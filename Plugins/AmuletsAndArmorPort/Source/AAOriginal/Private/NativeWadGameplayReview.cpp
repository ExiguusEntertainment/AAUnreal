#include "CoreMinimal.h"
#include "NativeWadReview.h"

#include "Algo/Find.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "NativeWadHudData.h"
#include "UnrealClient.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadInventoryView.h"
#include "WadSurfaceTextures.h"
#include "Engine/Texture2D.h"
#include "Misc/FileHelper.h"
#include "NativeWadScenery.h"
#include "NativeWadTown.h"
#include "Sound/SoundWave.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif

namespace
{
    const FKey ReviewRuneKeys[] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
        EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
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

    enum class ETownReview { None, Tour, Failure, Reload, Start, Scroll, Services, ServicesReload, ServicesLoadout, Inn, InnReload };
    enum class EUndeadReview { None, Launch, Castle, Traps, Keep, Boss, Return, Empty };
    enum class ETempleReview { None, Launch, L51, L52, Reload };
    enum class ECampaignAction { Place, Walk, Jump, Use, Floor, Ceiling, OpenDoor, HealthAtLeast, Pickup, Defeat, Frame,
        Rejuvenate, ManaAtLeast, StatueAngle, Checkpoint, Teleport, CeilingAtMost, TouchSector, DefeatCreature,
        UseFlightItem, Fly, JumpPickup, FlightEnded, CornerJump, UseLockedDoor, CrossDoor, FireWand, WaitBoulder };

    struct FCampaignAction
    {
        ECampaignAction Action;
        FVector Target;
        int32 Index = INDEX_NONE;
        const TCHAR* Label = nullptr;
        FVector2D Destination = FVector2D::ZeroVector;
    };

    struct FCampaignRoute
    {
        int32 Map, Quest, Destination, ExitThing;
        FVector2D Spawn;
        TConstArrayView<FCampaignAction> Actions;
    };

    struct FCampaignTurnIn
    {
        int32 Quest, FirstMap, Item, Gold, Experience;
    };

    constexpr const TCHAR* Campaign20ReturnSlot = TEXT("NativeWadCampaignL20ReturnReview");
    constexpr const TCHAR* Campaign60ReadySlot = TEXT("NativeWadCampaignL60ReadyReview");

    const FCampaignAction Campaign25[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 1800}, INDEX_NONE, TEXT("the same character recovers with Rejuvenate before entering the churchyard")},
        {ECampaignAction::Walk, {160, 1525, 0}}, {ECampaignAction::Use, {116, 1494, -155}},
        {ECampaignAction::OpenDoor, {0, 0, 100}, 145, TEXT("L25's entrance door opens from the original spawn")},
        {ECampaignAction::Walk, {100, 1466, 0}},
        {ECampaignAction::Walk, {-176, 775, 0}}, {ECampaignAction::Use, {-180, 744, -160}},
        {ECampaignAction::Floor, {0, 0, -208}, 158, TEXT("L25's bridge switch can be used from the entrance side")},
        {ECampaignAction::Walk, {-176, 680, 0}}, {ECampaignAction::Walk, {-400, 149, 0}},
        {ECampaignAction::Walk, {-352, 17, 0}}, {ECampaignAction::Use, {-327, -24, -50}},
        {ECampaignAction::OpenDoor, {0, 0, 120}, 160}, {ECampaignAction::Walk, {-288, -51, 0}},
        {ECampaignAction::Walk, {-64, -408, 0}}, {ECampaignAction::Walk, {-64, -462, 0}},
        {ECampaignAction::Ceiling, {0, 0, 72}, 100, TEXT("L25's plate opens the timed gate after the bridge crossing")},
        {ECampaignAction::Walk, {-64, -896, 0}}, {ECampaignAction::Walk, {-40, -1441, 0}},
        {ECampaignAction::Use, {-44, -1478, 48}}, {ECampaignAction::OpenDoor, {0, 0, 100}, 77},
        {ECampaignAction::Walk, {-40, -1915, 0}}, {ECampaignAction::Walk, {176, -2451, 0}},
        {ECampaignAction::Walk, {176, -2707, 0}}, {ECampaignAction::Walk, {225, -2707, 0}},
        {ECampaignAction::Floor, {0, 0, 112}, 15}, {ECampaignAction::Walk, {-155, -2772, 0}},
        {ECampaignAction::Ceiling, {0, 0, 240}, 130, TEXT("L25's two pressure plates raise the exit gate")},
        {ECampaignAction::Walk, {-8, -2700, 0}}, {ECampaignAction::Pickup, {-8, -2661, 146}, NativeWadRules::ChurchKey},
        {ECampaignAction::Rejuvenate, {0, 0, 760}, INDEX_NONE, TEXT("the character is ready to cross L25's exit hall")},
        {ECampaignAction::Walk, {-296, -2891, 0}}, {ECampaignAction::Walk, {-272, -3027, 0}},
        {ECampaignAction::Walk, {-96, -3115, 0}}, {ECampaignAction::Walk, {100, -3160, 0}},
        {ECampaignAction::Use, {120, -3190, 8}}, {ECampaignAction::OpenDoor, {0, 0, 100}, 138},
        {ECampaignAction::Walk, {108, -3235, 0}}, {ECampaignAction::Walk, {40, -3290, 0}},
        {ECampaignAction::Use, {9, -3290, 17}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 148, TEXT("L25's generated key opens the passage to L20")},
        {ECampaignAction::Walk, {-224, -3290, 0}},
    };

    const FCampaignAction Campaign20[] = {
        // Rest before leaving the entrance, then follow the west side into the switch wing.
        {ECampaignAction::Rejuvenate, {0, 0, 2000}, INDEX_NONE, TEXT("the carried character rests and heals inside L20's entrance")},
        {ECampaignAction::ManaAtLeast, {0, 0, 3200}, INDEX_NONE, TEXT("L20's entrance lets the carried character replenish mana")},
        {ECampaignAction::Walk, {-880, 4568, 0}}, {ECampaignAction::Walk, {-832, 4315, 0}},
        {ECampaignAction::Use, {-832, 4274, 28}}, {ECampaignAction::OpenDoor, {0, 0, 90}, 115},
        {ECampaignAction::Walk, {-848, 4136, 0}}, {ECampaignAction::Walk, {-1864, 3168, 0}},
        {ECampaignAction::Walk, {-2064, 2152, 0}}, {ECampaignAction::Walk, {-2248, 1592, 0}},
        {ECampaignAction::Walk, {-2240, 1464, 0}}, {ECampaignAction::Rejuvenate, {0, 0, 2000}},
        {ECampaignAction::Walk, {-2040, 1344, 0}},
        {ECampaignAction::Walk, {-1660, 1320, 0}}, {ECampaignAction::Use, {-1644, 1274, 45}},
        {ECampaignAction::OpenDoor, {0, 0, 75}, 89}, {ECampaignAction::Walk, {-1648, 1244, 0}},
        {ECampaignAction::Walk, {-1576, 1032, 0}}, {ECampaignAction::Walk, {-912, 1040, 0}},
        {ECampaignAction::Rejuvenate, {0, 0, 2100}}, {ECampaignAction::ManaAtLeast, {0, 0, 3450}},
        {ECampaignAction::Walk, {-872, 1240, 0}}, {ECampaignAction::Use, {-872, 1288, 126}},
        {ECampaignAction::OpenDoor, {0, 0, 100}, 92}, {ECampaignAction::Walk, {-872, 1352, 0}},
        {ECampaignAction::Walk, {-920, 1640, 0}}, {ECampaignAction::Walk, {-968, 1760, 0}},
        {ECampaignAction::Walk, {-832, 1996, 0}}, {ECampaignAction::Use, {-832, 2040, 117}},
        {ECampaignAction::OpenDoor, {0, 0, 80}, 117}, {ECampaignAction::Walk, {-832, 2104, 0}},
        {ECampaignAction::Walk, {-800, 2240, 0}}, {ECampaignAction::Use, {-800, 2279, 38}},
        {ECampaignAction::Ceiling, {0, 0, 112}, 18, TEXT("L20's timed entrance to the switch wing opens")},
        {ECampaignAction::Walk, {-800, 2320, 0}}, {ECampaignAction::Walk, {-896, 2400, 0}},
        {ECampaignAction::Walk, {-1048, 2416, 0}}, {ECampaignAction::Walk, {-1128, 2464, 0}},
        {ECampaignAction::Use, {-1128, 2503, 26}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 23},
        {ECampaignAction::Walk, {-1128, 2552, 0}}, {ECampaignAction::Walk, {-1128, 2580, 0}},
        {ECampaignAction::Use, {-1128, 2613, 32}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 26},
        {ECampaignAction::Walk, {-1128, 2656, 0}}, {ECampaignAction::Walk, {-1256, 2704, 0}},
        {ECampaignAction::Walk, {-1304, 2704, 0}}, {ECampaignAction::Walk, {-1392, 2552, 0}},
        {ECampaignAction::Use, {-1392, 2516, 24}}, {ECampaignAction::Ceiling, {0, 0, 100}, 29},
        {ECampaignAction::ManaAtLeast, {0, 0, 2750}}, {ECampaignAction::Rejuvenate, {0, 0, 2050}},
        {ECampaignAction::ManaAtLeast, {0, 0, 3450}},
        {ECampaignAction::Walk, {-1296, 2720, 0}}, {ECampaignAction::Walk, {-728, 2720, 0}},
        {ECampaignAction::Use, {-688, 2719, 76}}, {ECampaignAction::Ceiling, {0, 0, 84}, 20},
        {ECampaignAction::Walk, {-1048, 2704, 0}}, {ECampaignAction::Walk, {-1128, 2656, 0}},
        {ECampaignAction::Use, {-1128, 2613, 32}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 26},
        {ECampaignAction::Walk, {-1128, 2552, 0}}, {ECampaignAction::Walk, {-1128, 2536, 0}},
        {ECampaignAction::Use, {-1128, 2503, 26}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 23},
        {ECampaignAction::Walk, {-1096, 2464, 0}}, {ECampaignAction::Walk, {-1008, 2400, 0}},
        {ECampaignAction::Walk, {-848, 2424, 0}}, {ECampaignAction::Walk, {-816, 2488, 0}},
        {ECampaignAction::Walk, {-1030, 2558, 0}}, {ECampaignAction::Use, {-1068, 2558, 16}},
        {ECampaignAction::Ceiling, {0, 0, 230}, 107, TEXT("L20's switch opens the exit approach")},
        {ECampaignAction::Walk, {-656, 2608, 0}}, {ECampaignAction::Walk, {-624, 2760, 0}},
        {ECampaignAction::Use, {-624, 2800, 16}}, {ECampaignAction::Ceiling, {0, 0, 88}, 15},
        {ECampaignAction::Walk, {-664, 2600, 0}}, {ECampaignAction::Walk, {-796, 2488, 0}},
        {ECampaignAction::Walk, {-796, 2456, 0}},
        {ECampaignAction::Walk, {-704, 2416, 0}}, {ECampaignAction::Walk, {-112, 2544, 0}},
        // Reopen the timed gate from inside and jump onto the stepped return path.
        {ECampaignAction::Walk, {0, 2360, 0}}, {ECampaignAction::Use, {3, 2320, 53}},
        {ECampaignAction::Ceiling, {0, 0, 112}, 18, TEXT("L20's inside switch reopens the timed entrance")},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, Campaign20ReturnSlot},
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3300}},
        {ECampaignAction::Walk, {-128, 2528, 0}}, {ECampaignAction::Walk, {-376, 2496, 0}},
        {ECampaignAction::Walk, {-792, 2296, 0}}, {ECampaignAction::Walk, {-936, 2216, 0}},
        {ECampaignAction::Walk, {-960, 2216, 0}},
        {ECampaignAction::Jump, {-960, 2144, 0}, 56, TEXT("the player jumps onto L20's return step")},
        {ECampaignAction::Walk, {-864, 2096, 0}}, {ECampaignAction::Walk, {-832, 2074, 0}},
        {ECampaignAction::Use, {-832, 2043, 117}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 117},
        {ECampaignAction::Walk, {-832, 1992, 0}}, {ECampaignAction::Walk, {-536, 1656, 0}},
        {ECampaignAction::Walk, {-64, 1640, 0}},
        {ECampaignAction::Walk, {0, 1690, 0}},
        {ECampaignAction::Ceiling, {0, 0, 190}, 87, TEXT("L20's pressure plate opens the original exit")},
        {ECampaignAction::Walk, {-592, 1712, 0}}, {ECampaignAction::Walk, {-800, 1992, 0}},
        {ECampaignAction::Use, {-800, 2040, 117}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 117},
        {ECampaignAction::Walk, {-776, 2096, 0}}, {ECampaignAction::Walk, {-728, 2120, 0}},
        {ECampaignAction::Walk, {-680, 2048, 0}}, {ECampaignAction::Walk, {-512, 1848, 0}},
        {ECampaignAction::Walk, {-376, 1816, 0}}, {ECampaignAction::Walk, {30, 1792, 0}},
        {ECampaignAction::Walk, {196, 1792, 0}}, {ECampaignAction::Walk, {196, 1888, 0}},
        {ECampaignAction::Walk, {212, 1903, 0}}, {ECampaignAction::Walk, {212, 1946, 0}},
    };

    constexpr int32 HealingPotion = 800;
    constexpr int32 GreaterHealingPotion = 801;
    constexpr int32 IronLongSword = 204;
    constexpr int32 MagicDartsWand = 900;

    int32 BestReviewMeleeWeapon(const ANativeWadPlayer& Player)
    {
        const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
        int32 Weapon = 0;
        int32 Damage = 0;
        if (Catalog)
        {
            for (const int32 Type : Player.GetCarriedItems())
            {
                const FWadInventoryDetails* Item = Catalog->ItemDetails.Find(Type);
                if (Item && Item->HitFrame > 0 && Item->WeaponDamage > Damage && Player.CanUseWadItem(Type))
                {
                    Weapon = Type;
                    Damage = Item->WeaponDamage;
                }
            }
        }
        return Weapon;
    }
    constexpr const TCHAR* LighthouseReturnSlot = TEXT("NativeWadCampaignL34ReturnReview");
    const FCampaignAction Campaign60[] = {
        // The three entrance doors lead around to the lower pools.
        {ECampaignAction::Rejuvenate, {0, 0, 1800}}, {ECampaignAction::ManaAtLeast, {0, 0, 3000}},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, Campaign60ReadySlot},
        {ECampaignAction::Walk, {-2888, -10, 0}}, {ECampaignAction::Use, {-2848, -10, 128}},
        {ECampaignAction::OpenDoor, {0, 0, 100}, 192, TEXT("L60's entrance opens from the original spawn")},
        {ECampaignAction::Walk, {-2699, -48, 0}}, {ECampaignAction::Walk, {-2611, -156, 0}},
        {ECampaignAction::Walk, {-1816, -156, 0}}, {ECampaignAction::Walk, {-1755, -24, 0}},
        {ECampaignAction::Walk, {-1710, -24, 0}}, {ECampaignAction::Jump, {-1608, -24, 0}, 76},
        {ECampaignAction::Walk, {-843, -24, 0}}, {ECampaignAction::Walk, {-537, -24, 0}},
        {ECampaignAction::Use, {-496, -24, 168}}, {ECampaignAction::OpenDoor, {0, 0, 100}, 17},
        {ECampaignAction::Walk, {-411, -48, 0}}, {ECampaignAction::Walk, {-6, -432, 0}},
        {ECampaignAction::Use, {-6, -472, 166}}, {ECampaignAction::OpenDoor, {0, 0, 100}, 250},
        {ECampaignAction::Walk, {-6, -523, 0}}, {ECampaignAction::Walk, {477, -960, 0}},
        {ECampaignAction::Walk, {1117, -912, 0}}, {ECampaignAction::Walk, {1333, -736, 0}},
        {ECampaignAction::Walk, {1429, -752, 0}}, {ECampaignAction::Walk, {1453, -792, 0}},
        {ECampaignAction::Walk, {1469, -936, 0}}, {ECampaignAction::Walk, {1493, -960, 0}},
        {ECampaignAction::Walk, {2509, -960, 0}}, {ECampaignAction::Walk, {2621, -944, 0}},
        {ECampaignAction::Walk, {2645, -896, 0}}, {ECampaignAction::Walk, {2637, -288, 0}},
        {ECampaignAction::Walk, {2629, -280, 0}}, {ECampaignAction::Walk, {2485, -168, 0}},
        {ECampaignAction::Walk, {2421, -112, 0}}, {ECampaignAction::Walk, {2381, -72, 0}},
        {ECampaignAction::Walk, {2237, 152, 0}}, {ECampaignAction::Walk, {1957, 192, 0}},
        {ECampaignAction::Walk, {1933, 232, 0}}, {ECampaignAction::Walk, {1949, 1144, 0}},
        {ECampaignAction::Walk, {1957, 1152, 0}}, {ECampaignAction::Walk, {2085, 1144, 0}},
        {ECampaignAction::Walk, {2189, 1056, 0}},
        // Step off each plate between turns and walk around solved plates.
        {ECampaignAction::Walk, {2234, 1056, 0}}, {ECampaignAction::StatueAngle, {0, 0, 49152}, 297},
        {ECampaignAction::Walk, {2183, 1056, 0}}, {ECampaignAction::Walk, {2234, 1056, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 0}, 297, TEXT("L60's first statue faces the exit")},
        {ECampaignAction::Walk, {2183, 1056, 0}}, {ECampaignAction::Walk, {1869, 1208, 0}},
        {ECampaignAction::Walk, {1757, 1680, 0}}, {ECampaignAction::Walk, {1775, 1696, 0}},
        {ECampaignAction::Walk, {1818, 1696, 0}}, {ECampaignAction::StatueAngle, {0, 0, 16384}, 293},
        {ECampaignAction::Walk, {1767, 1696, 0}}, {ECampaignAction::Walk, {1767, 1616, 0}},
        {ECampaignAction::Walk, {1805, 1592, 0}}, {ECampaignAction::Walk, {2381, 1624, 0}},
        {ECampaignAction::Walk, {2447, 1600, 0}}, {ECampaignAction::Walk, {2490, 1600, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 49152}, 294},
        {ECampaignAction::Walk, {2439, 1600, 0}}, {ECampaignAction::Walk, {2490, 1600, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 0}, 294},
        {ECampaignAction::Walk, {2439, 1600, 0}}, {ECampaignAction::Walk, {2439, 1720, 0}},
        {ECampaignAction::Walk, {2517, 1952, 0}}, {ECampaignAction::Walk, {2717, 2136, 0}},
        {ECampaignAction::Walk, {2813, 2120, 0}}, {ECampaignAction::Walk, {2989, 1272, 0}},
        {ECampaignAction::Walk, {2973, 1232, 0}}, {ECampaignAction::Walk, {2853, 1112, 0}},
        {ECampaignAction::Walk, {2855, 1056, 0}}, {ECampaignAction::Walk, {2906, 1056, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 0}, 296},
        {ECampaignAction::Walk, {2855, 1056, 0}}, {ECampaignAction::Walk, {2855, 1148, 0}},
        {ECampaignAction::Walk, {2917, 1184, 0}}, {ECampaignAction::Walk, {2997, 1248, 0}},
        {ECampaignAction::Walk, {3111, 1856, 0}}, {ECampaignAction::Walk, {3162, 1856, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 32768}, 295},
        {ECampaignAction::Walk, {3111, 1856, 0}}, {ECampaignAction::Walk, {3162, 1856, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 49152}, 295},
        {ECampaignAction::Walk, {3111, 1856, 0}}, {ECampaignAction::Walk, {3162, 1856, 0}},
        {ECampaignAction::StatueAngle, {0, 0, 0}, 295},
        {ECampaignAction::Walk, {3111, 1856, 0}},
        {ECampaignAction::Floor, {0, 0, 67}, 141, TEXT("L60's five statues raise the original crossing")},
        // Follow the raised stairs to the relic and take the original exit to Town Hall.
        {ECampaignAction::Walk, {3013, 2048, 0}}, {ECampaignAction::Walk, {2797, 2048, 0}},
        {ECampaignAction::Walk, {2789, 2000, 0}}, {ECampaignAction::Walk, {2741, 1872, 0}},
        {ECampaignAction::Walk, {2701, 1848, 0}}, {ECampaignAction::Walk, {2630, 1850, 0}},
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3350}},
        {ECampaignAction::Walk, {2301, 1864, 0}},
        {ECampaignAction::Walk, {2277, 1888, 0}}, {ECampaignAction::Walk, {2293, 2264, 0}},
        {ECampaignAction::Walk, {2325, 2280, 0}}, {ECampaignAction::Walk, {3005, 2296, 0}},
        {ECampaignAction::Walk, {3029, 2320, 0}}, {ECampaignAction::Walk, {3013, 2704, 0}},
        {ECampaignAction::Walk, {2813, 2904, 0}}, {ECampaignAction::Walk, {2813, 2920, 0}},
        {ECampaignAction::Walk, {2837, 2976, 0}}, {ECampaignAction::Walk, {2909, 3048, 0}},
        {ECampaignAction::Walk, {2965, 3072, 0}}, {ECampaignAction::Walk, {3309, 3064, 0}},
        {ECampaignAction::Walk, {3333, 3088, 0}}, {ECampaignAction::Walk, {3330, 3208, 0}},
        {ECampaignAction::Walk, {3261, 3216, 0}}, {ECampaignAction::Walk, {3170, 3232, 0}},
        {ECampaignAction::Walk, {3170, 3320, 0}},
        {ECampaignAction::Rejuvenate, {0, 0, 2000}}, {ECampaignAction::ManaAtLeast, {0, 0, 2300}},
        {ECampaignAction::Walk, {3179, 3501, 0}},
        {ECampaignAction::Walk, {3221, 3536, 0}}, {ECampaignAction::Walk, {3975, 3518, 0}},
        {ECampaignAction::Walk, {4034, 3535, 0}},
        {ECampaignAction::Pickup, {4034, 3573, 330}, GreaterHealingPotion, TEXT("the first authored L60 healing potion")},
        {ECampaignAction::Pickup, {4034, 3500, 330}, GreaterHealingPotion, TEXT("the second authored L60 healing potion")},
        {ECampaignAction::Pickup, {4020, 3516, 330}, NativeWadRules::AndrewNecklace, TEXT("the original Necklace of Andrew pickup")},
        {ECampaignAction::Floor, {0, 0, 290}, 247, TEXT("L60's pedestal opens the original exit gate")},
        {ECampaignAction::Walk, {4001, 3561, 0}}, {ECampaignAction::Walk, {3900, 3561, 0}},
        {ECampaignAction::Walk, {3660, 3561, 0}}, {ECampaignAction::Walk, {3550, 3561, 0}},
        {ECampaignAction::Walk, {3441, 3561, 0}}, {ECampaignAction::Walk, {3441, 3625, 0}},
        {ECampaignAction::Walk, {3427, 3664, 0}}, {ECampaignAction::Walk, {3383, 3708, 0}},
        {ECampaignAction::Walk, {3328, 3714, 0}},
    };

    const FCampaignAction Campaign51[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2800}}, {ECampaignAction::ManaAtLeast, {0, 0, 4200}},
        {ECampaignAction::Use, {-162, -138, 139}}, {ECampaignAction::Floor, {0, 0, 96}, 138},
        {ECampaignAction::Walk, {-457, 315, 0}}, {ECampaignAction::Walk, {-1145, 699, 0}},
        {ECampaignAction::Walk, {-1993, 739, 0}}, {ECampaignAction::Walk, {-2025, 803, 0}},
        {ECampaignAction::Walk, {-2025, 1819, 0}}, {ECampaignAction::Walk, {-1865, 2339, 0}},
        {ECampaignAction::Walk, {-849, 2915, 0}}, {ECampaignAction::Walk, {-584, 2940, 0}},
        {ECampaignAction::Jump, {-460, 2940, 0}, 123},
        {ECampaignAction::TouchSector, {-350, 2945, 0}, 49},
        {ECampaignAction::Floor, {0, 0, 0}, 35, TEXT("L51's pressure plate lowers the bridge")},
        {ECampaignAction::Walk, {-641, 2403, 0}}, {ECampaignAction::Walk, {-848, 2344, 0}},
        {ECampaignAction::Use, {-848, 2304, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 113},
        {ECampaignAction::Walk, {-919, 1643, 0}}, {ECampaignAction::Jump, {-998, 1575, 0}, 100},
        {ECampaignAction::TouchSector, {-1050, 1510, 0}, 70},
        {ECampaignAction::Floor, {0, 0, 200}, 144, TEXT("L51's upper plate raises the two-part bridge")},
        {ECampaignAction::Floor, {0, 0, 200}, 124},
        {ECampaignAction::Walk, {-1169, 1019, 0}}, {ECampaignAction::Walk, {-1169, 1011, 0}},
        {ECampaignAction::Walk, {-1081, 963, 0}}, {ECampaignAction::Walk, {-1012, 950, 0}},
        {ECampaignAction::Use, {-970, 950, 197}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 81},
        {ECampaignAction::Walk, {-919, 950, 0}}, {ECampaignAction::Walk, {-330, 872, 0}},
        {ECampaignAction::Use, {-288, 872, 213}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 29},
        {ECampaignAction::Walk, {-225, 875, 0}},
        {ECampaignAction::Walk, {-41, 1099, 0}}, {ECampaignAction::Walk, {-113, 2027, 0}},
        {ECampaignAction::Walk, {-214, 2147, 0}}, {ECampaignAction::Use, {-256, 2147, 214}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 19}, {ECampaignAction::Walk, {-308, 2147, 0}},
        {ECampaignAction::Walk, {-1273, 2203, 0}}, {ECampaignAction::Walk, {-1791, 2202, 0}},
        {ECampaignAction::Use, {-1833, 2202, 245}},
        {ECampaignAction::Ceiling, {0, 0, 168}, 115, TEXT("L51's west switch raises the exit gate")},
        {ECampaignAction::Walk, {-777, 2147, 0}}, {ECampaignAction::Walk, {-315, 2131, 0}},
        {ECampaignAction::Use, {-273, 2131, 244}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 19},
        {ECampaignAction::Walk, {-217, 2131, 0}},
        {ECampaignAction::Walk, {-65, 1931, 0}}, {ECampaignAction::Walk, {-129, 1499, 0}},
        {ECampaignAction::Walk, {-157, 1466, 0}},
        {ECampaignAction::Use, {-157, 1424, 140}}, {ECampaignAction::Floor, {0, 0, 0}, 144},
        {ECampaignAction::Floor, {0, 0, 0}, 124}, {ECampaignAction::Walk, {-848, 2231, 0}},
        {ECampaignAction::Use, {-848, 2288, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 113},
        {ECampaignAction::Walk, {-969, 2403, 0}},
        {ECampaignAction::Walk, {-1049, 2427, 0}}, {ECampaignAction::Walk, {-1244, 2781, 0}},
        {ECampaignAction::Jump, {-1309, 2860, 0}, 95}, {ECampaignAction::Walk, {-1366, 2948, 0}},
        {ECampaignAction::Use, {-1408, 2948, 93}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 53},
        {ECampaignAction::Walk, {-1473, 3043, 0}}, {ECampaignAction::Walk, {-1466, 3252, 0}},
        {ECampaignAction::Use, {-1424, 3252, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 57},
        {ECampaignAction::Walk, {-1329, 3274, 0}}, {ECampaignAction::Walk, {-1329, 3350, 0}},
        {ECampaignAction::Use, {-1329, 3392, 51}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 56},
        {ECampaignAction::Walk, {-1329, 3430, 0}}, {ECampaignAction::Walk, {-1777, 3643, 0}},
        {ECampaignAction::Walk, {-2104, 3676, 0}}, {ECampaignAction::Pickup, {-2104, 3714, 29}, GreaterHealingPotion},
        {ECampaignAction::Walk, {-2075, 3712, 0}}, {ECampaignAction::Pickup, {-2040, 3742, 29}, HealingPotion},
        {ECampaignAction::Walk, {-2108, 3590, 0}}, {ECampaignAction::Pickup, {-2108, 3556, 29}, GreaterHealingPotion},
        {ECampaignAction::Walk, {-2080, 3560, 0}}, {ECampaignAction::Pickup, {-2044, 3536, 29}, HealingPotion},
        {ECampaignAction::Walk, {-2272, 3642, 0}},
    };
    const FCampaignAction Campaign52Exit[] = {
        {ECampaignAction::Walk, {-3448, 1440, 0}},
    };
    const FCampaignAction Campaign52[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2800}}, {ECampaignAction::ManaAtLeast, {0, 0, 4200}},
        {ECampaignAction::Walk, {520, 3408, 0}}, {ECampaignAction::Walk, {152, 3472, 0}},
        {ECampaignAction::Walk, {-792, 3824, 0}}, {ECampaignAction::TouchSector, {-1360, 3670, 0}, 106},
        {ECampaignAction::Ceiling, {0, 0, 25}, 94, TEXT("L52's first plate opens the gate to Z25")},
        {ECampaignAction::TouchSector, {-1830, 3552, 0}, 96},
        {ECampaignAction::Ceiling, {0, 0, 50}, 94, TEXT("L52's second plate opens the gate to Z50")},
        {ECampaignAction::TouchSector, {-944, 3248, 0}, 97},
        {ECampaignAction::Ceiling, {0, 0, 75}, 94, TEXT("L52's third plate opens the gate to Z75")},
        {ECampaignAction::Walk, {-1400, 3120, 0}}, {ECampaignAction::Walk, {-2416, 2104, 0}},
        {ECampaignAction::Walk, {-2472, 1088, 0}}, {ECampaignAction::Walk, {-2480, 440, 0}},
        {ECampaignAction::Walk, {-2568, 432, 0}}, {ECampaignAction::Walk, {-3128, 1424, 0}},
        {ECampaignAction::Walk, {-3442, 1467, 0}},
    };
    const FCampaignAction Campaign41[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2800}}, {ECampaignAction::ManaAtLeast, {0, 0, 4200}},
        {ECampaignAction::Walk, {6730, 4160, 0}}, {ECampaignAction::Use, {6688, 4160, -85}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 320}, {ECampaignAction::Walk, {6088, 4160, 0}},
        {ECampaignAction::Walk, {5512, 4184, 0}}, {ECampaignAction::Walk, {5440, 4296, 0}},
        {ECampaignAction::Walk, {5392, 4624, 0}}, {ECampaignAction::Walk, {4568, 4616, 0}},
        {ECampaignAction::Walk, {4312, 4336, 0}}, {ECampaignAction::Walk, {4336, 4152, 0}},
        {ECampaignAction::Walk, {4640, 3872, 0}}, {ECampaignAction::Walk, {4640, 3370, 0}},
        {ECampaignAction::Use, {4640, 3328, 58}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 11},
        {ECampaignAction::Walk, {4640, 2856, 0}},
        {ECampaignAction::Walk, {4458, 2464, 0}}, {ECampaignAction::Use, {4416, 2464, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 285}, {ECampaignAction::Walk, {4312, 2464, 0}},
        {ECampaignAction::Walk, {3984, 2112, 0}}, {ECampaignAction::Walk, {3224, 2160, 0}},
        {ECampaignAction::Walk, {2976, 2326, 0}}, {ECampaignAction::Use, {2976, 2368, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 45}, {ECampaignAction::Walk, {2976, 2656, 0}},
        {ECampaignAction::Walk, {2976, 2902, 0}}, {ECampaignAction::Use, {2976, 2944, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 49}, {ECampaignAction::Walk, {2976, 3672, 0}},
        {ECampaignAction::Walk, {2976, 4688, 0}}, {ECampaignAction::Walk, {2976, 4928, 0}},
        {ECampaignAction::JumpPickup, {2976, 4961, 46}, NativeWadRules::Key9, TEXT("the original L41 Key 9")},
        {ECampaignAction::Walk, {2976, 3904, 0}}, {ECampaignAction::Walk, {2976, 3050, 0}},
        {ECampaignAction::Use, {2976, 3008, 34}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 49},
        {ECampaignAction::Walk, {2976, 2474, 0}}, {ECampaignAction::Use, {2976, 2432, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 45}, {ECampaignAction::Walk, {3184, 2128, 0}},
        {ECampaignAction::Walk, {3248, 2064, 0}},
        {ECampaignAction::Walk, {3160, 1048, 0}}, {ECampaignAction::Walk, {3216, 840, 0}},
        // Use the authored teleports to reach the west side of the maze.
        {ECampaignAction::Walk, {2950, 852, 0}}, {ECampaignAction::Walk, {2775, 852, 0}},
        {ECampaignAction::Walk, {2602, 827, 0}}, {ECampaignAction::Use, {2560, 827, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 51}, {ECampaignAction::Walk, {2480, 784, 0}},
        {ECampaignAction::Walk, {2320, 440, 0}},
        {ECampaignAction::Walk, {2288, 424, 0}}, {ECampaignAction::Walk, {2232, 432, 0}},
        {ECampaignAction::Walk, {2144, 608, 0}},
        {ECampaignAction::Teleport, {2144, 1032, 0}, INDEX_NONE, nullptr, {1634, 379}},
        {ECampaignAction::Teleport, {1632, 1032, 0}, INDEX_NONE, nullptr, {1124, 379}},
        {ECampaignAction::Walk, {1304, 440, 0}},
        {ECampaignAction::Teleport, {1376, 1032, 0}, INDEX_NONE, TEXT("L41's maze leads to the key wing"), {852, 379}},
        {ECampaignAction::Walk, {810, 341, 0}}, {ECampaignAction::Use, {768, 341, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 65}, {ECampaignAction::Walk, {-96, 320, 0}},
        {ECampaignAction::Walk, {-168, 408, 0}},
        {ECampaignAction::Walk, {-144, 584, 0}}, {ECampaignAction::Walk, {-64, 624, 0}},
        {ECampaignAction::Walk, {304, 656, 0}}, {ECampaignAction::Walk, {368, 1685, 0}},
        {ECampaignAction::Use, {368, 1728, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 92},
        {ECampaignAction::Walk, {368, 1864, 0}}, {ECampaignAction::Walk, {64, 1944, 0}},
        {ECampaignAction::TouchSector, {188, 2044, 0}, 346},
        {ECampaignAction::Walk, {82, 2044, 0}}, {ECampaignAction::Walk, {82, 2300, 0}},
        {ECampaignAction::TouchSector, {188, 2300, 0}, 98},
        {ECampaignAction::Walk, {82, 2300, 0}}, {ECampaignAction::Walk, {82, 2530, 0}},
        {ECampaignAction::Walk, {316, 2530, 0}}, {ECampaignAction::TouchSector, {316, 2428, 0}, 103},
        {ECampaignAction::Walk, {316, 2530, 0}}, {ECampaignAction::Walk, {82, 2530, 0}},
        {ECampaignAction::Walk, {87, 1943, 0}}, {ECampaignAction::Walk, {183, 2039, 0}},
        {ECampaignAction::CornerJump, {319, 2175, 0}, 65, nullptr, {274, 2102}},
        {ECampaignAction::Ceiling, {0, 0, 140}, 112, TEXT("L41's four correct plates open the Key 6 gate")},
        {ECampaignAction::Walk, {64, 2615, 0}}, {ECampaignAction::Walk, {64, 2850, 0}},
        {ECampaignAction::Walk, {-76, 2942, 0}}, {ECampaignAction::Pickup, {-76, 2910, 52}, NativeWadRules::ElmoreKey6},
        {ECampaignAction::Walk, {64, 2850, 0}}, {ECampaignAction::Walk, {64, 2615, 0}},
        {ECampaignAction::Walk, {64, 1872, 0}}, {ECampaignAction::Walk, {368, 1864, 0}},
        {ECampaignAction::Use, {368, 1824, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 92},
        {ECampaignAction::Walk, {312, 664, 0}}, {ECampaignAction::Walk, {152, 624, 0}},
        {ECampaignAction::Walk, {-136, 592, 0}}, {ECampaignAction::Walk, {-168, 528, 0}},
        {ECampaignAction::Walk, {-144, 368, 0}}, {ECampaignAction::Walk, {662, 341, 0}},
        {ECampaignAction::Use, {704, 341, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 65},
        {ECampaignAction::Walk, {784, 341, 0}},
        {ECampaignAction::Teleport, {864, 1032, 0}, INDEX_NONE, nullptr, {1634, 379}},
        {ECampaignAction::Walk, {1816, 440, 0}},
        {ECampaignAction::Teleport, {1888, 1032, 0}, INDEX_NONE, nullptr, {2144, 379}},
        {ECampaignAction::Walk, {2232, 432, 0}}, {ECampaignAction::Walk, {2288, 424, 0}},
        {ECampaignAction::Walk, {2320, 440, 0}}, {ECampaignAction::Walk, {2454, 827, 0}},
        {ECampaignAction::Use, {2496, 827, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 51},
        {ECampaignAction::Walk, {2775, 852, 0}}, {ECampaignAction::Walk, {2950, 852, 0}},
        {ECampaignAction::Walk, {3621, 832, 0}}, {ECampaignAction::Use, {3677, 832, 40}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 37, TEXT("L41 Key 6 opens door 37")},
        {ECampaignAction::Walk, {4096, 768, 0}}, {ECampaignAction::TouchSector, {4384, 608, 0}, 30},
        {ECampaignAction::Ceiling, {0, 0, 80}, 204},
        {ECampaignAction::Walk, {3984, 432, 0}}, {ECampaignAction::Walk, {3344, 328, 0}},
        {ECampaignAction::Walk, {3264, 152, 0}}, {ECampaignAction::Walk, {3704, -840, 0}},
        {ECampaignAction::Walk, {3704, -1856, 0}}, {ECampaignAction::Walk, {3552, -2018, 0}},
        {ECampaignAction::Use, {3552, -2048, 42}},
        {ECampaignAction::Ceiling, {0, 0, 130}, 220, TEXT("L41 timed switch raises gate 220 to Z130")},
        {ECampaignAction::Walk, {3704, -1312, 0}}, {ECampaignAction::Walk, {3528, -816, 0}},
        {ECampaignAction::Walk, {3352, -720, 0}}, {ECampaignAction::Walk, {3320, -720, 0}},
        {ECampaignAction::Walk, {3200, -960, 0}}, {ECampaignAction::Walk, {3200, -1976, 0}},
        {ECampaignAction::Walk, {3248, -2768, 0}}, {ECampaignAction::Walk, {3856, -2864, 0}},
        {ECampaignAction::Teleport, {3996, -3044, 0}, INDEX_NONE, nullptr, {3872, 3747}},
        {ECampaignAction::Walk, {3872, 4022, 0}}, {ECampaignAction::Use, {3872, 4064, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 158}, {ECampaignAction::Walk, {3872, 4166, 0}},
        {ECampaignAction::Walk, {3680, 4760, 0}}, {ECampaignAction::Walk, {3728, 5008, 0}},
        {ECampaignAction::Walk, {4744, 5080, 0}}, {ECampaignAction::Walk, {5648, 5168, 0}},
        {ECampaignAction::Walk, {5824, 5608, 0}}, {ECampaignAction::Walk, {6504, 6288, 0}},
        {ECampaignAction::Walk, {6688, 6448, 0}}, {ECampaignAction::Walk, {6408, 7416, 0}},
        {ECampaignAction::Teleport, {6336, 7648, 0}, INDEX_NONE, nullptr, {5297, -393}},
        {ECampaignAction::Walk, {5296, 624, 0}},
        {ECampaignAction::Teleport, {5300, 732, 0}, INDEX_NONE, nullptr, {1344, 3746}},
        {ECampaignAction::Walk, {1536, 4128, 0}}, {ECampaignAction::Walk, {1816, 4216, 0}},
        {ECampaignAction::Walk, {1856, 4956, 0}}, {ECampaignAction::Floor, {0, 0, 0}, 271},
        {ECampaignAction::Walk, {1856, 5232, 0}}, {ECampaignAction::Walk, {1856, 6248, 0}},
        {ECampaignAction::Walk, {1808, 6544, 0}}, {ECampaignAction::Walk, {1360, 6688, 0}},
        {ECampaignAction::Walk, {1352, 6736, 0}},
        {ECampaignAction::Walk, {1352, 6784, 0}},
    };
    const FCampaignAction Campaign44Combat[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3200}}, {ECampaignAction::ManaAtLeast, {0, 0, 4300}},
        // Complete the other two plates after the fight so the timed gate stays open during the return.
        {ECampaignAction::Place, {-268, 1046, 0}},
        {ECampaignAction::Defeat, {}, NativeWadRules::ElymasNotes, TEXT("Elymas")},
        {ECampaignAction::Place, {-714, -414, 0}}, {ECampaignAction::Walk, {-650, -414, 0}},
        {ECampaignAction::Floor, {0, 0, 320}, 108},
        {ECampaignAction::Place, {54, -416, 0}}, {ECampaignAction::Walk, {-8, -416, 0}},
        {ECampaignAction::Floor, {0, 0, 320}, 107},
        {ECampaignAction::Ceiling, {0, 0, 112}, 68, TEXT("L44 three pressure plates raise gate 68")},
        {ECampaignAction::Place, {-714, 3060, 0}}, {ECampaignAction::Pickup, {-702, 3090, 44}, NativeWadRules::ElmoreKey3},
        {ECampaignAction::Place, {384, 3166, 0}}, {ECampaignAction::Use, {384, 3136, 42}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 42, TEXT("L44 Key 3 opens the Town Hall passage")},
        {ECampaignAction::Place, {408, 3264, 0}}, {ECampaignAction::Walk, {384, 3296, 0}},
    };
    const FCampaignAction Campaign44[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3200}}, {ECampaignAction::ManaAtLeast, {0, 0, 4300}},
        {ECampaignAction::Walk, {-3274, 762, 0}}, {ECampaignAction::Use, {-3232, 762, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 93}, {ECampaignAction::Walk, {-3152, 704, 0}},
        {ECampaignAction::Walk, {-3008, 520, 0}}, {ECampaignAction::Walk, {-2952, 296, 0}},
        {ECampaignAction::Walk, {-1968, 304, 0}}, {ECampaignAction::Walk, {-1840, 752, 0}},
        {ECampaignAction::Walk, {-1744, 824, 0}}, {ECampaignAction::Walk, {-1514, 822, 0}},
        {ECampaignAction::Use, {-1472, 822, 75}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 47},
        {ECampaignAction::Walk, {-1392, 784, 0}}, {ECampaignAction::Walk, {-1296, -232, 0}},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {-1288, -320, 170}}, {ECampaignAction::Fly, {-1160, -552, 277}},
        {ECampaignAction::Fly, {-956, -1040, 282}}, {ECampaignAction::Fly, {-956, -1040, 376}},
        {ECampaignAction::Fly, {-272, -184, 376}, INDEX_NONE, TEXT("L44's first potion reaches the high stairway")},
        {ECampaignAction::Fly, {-272, -184, 586}}, {ECampaignAction::Fly, {-272, 640, 586}},
        {ECampaignAction::DefeatCreature, {}, 50, TEXT("L44's Electric Skeleton at the high stairway")},
        {ECampaignAction::DefeatCreature, {}, 35, TEXT("L44's western Electric Skeleton guard")},
        {ECampaignAction::DefeatCreature, {}, 34, TEXT("L44's eastern Electric Skeleton guard")},
        {ECampaignAction::Fly, {-268, 1046, 580}}, {ECampaignAction::Floor, {0, 0, 512}, 102},
        {ECampaignAction::Defeat, {}, NativeWadRules::ElymasNotes, TEXT("Elymas")},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {-650, -414, 575}}, {ECampaignAction::Floor, {0, 0, 320}, 108},
        {ECampaignAction::Fly, {-8, -416, 555}}, {ECampaignAction::Floor, {0, 0, 320}, 107},
        {ECampaignAction::Ceiling, {0, 0, 112}, 68, TEXT("L44 three pressure plates raise gate 68")},
        {ECampaignAction::Fly, {56, 600, 400}}, {ECampaignAction::Fly, {160, 1384, 415}},
        {ECampaignAction::Fly, {160, 1384, 65}}, {ECampaignAction::Fly, {-88, 2384, 64}},
        {ECampaignAction::Fly, {-116, 2448, 60}}, {ECampaignAction::Fly, {-352, 2840, 58}},
        {ECampaignAction::Use, {-352, 2896, 42}}, {ECampaignAction::OpenDoor, {0, 0, 64}, 68},
        {ECampaignAction::Fly, {-368, 3016, 55}}, {ECampaignAction::Fly, {-416, 3040, 55}},
        {ECampaignAction::Fly, {-446, 3040, 56}}, {ECampaignAction::Use, {-488, 3040, 62}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 96}, {ECampaignAction::Fly, {-558, 3040, 55}},
        {ECampaignAction::Fly, {-714, 3060, 64}},
        {ECampaignAction::Pickup, {-702, 3090, 44}, NativeWadRules::ElmoreKey3, TEXT("the original L44 Key 3")},
        {ECampaignAction::CrossDoor, {-440, 3040, 62}, 96, TEXT("L44's Key 3 vault door on the way back"), {-552, 3040}},
        {ECampaignAction::Fly, {-376, 3024, 63}}, {ECampaignAction::Fly, {-352, 2930, 55}},
        {ECampaignAction::UseLockedDoor, {-352, 2896, 42}, 68},
        {ECampaignAction::Fly, {-328, 2808, 64}}, {ECampaignAction::Fly, {208, 2544, 64}},
        {ECampaignAction::Fly, {384, 2790, 69}}, {ECampaignAction::Fly, {384, 3088, 54}},
        {ECampaignAction::UseLockedDoor, {384, 3104, 66}, 42, TEXT("L44 Key 3 opens the Town Hall passage")},
        {ECampaignAction::Walk, {384, 3166, 0}}, {ECampaignAction::Walk, {384, 3296, 0}},
    };
    const FCampaignAction Campaign74[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2800}},
        {ECampaignAction::ManaAtLeast, {0, 0, 4200}},
        {ECampaignAction::Walk, {-1120, -216, 0}}, {ECampaignAction::Use, {-1120, -176, 30}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 188}, {ECampaignAction::Walk, {-1120, 574, 0}},
        {ECampaignAction::Use, {-1120, 618, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 47},
        {ECampaignAction::Walk, {-1113, 698, 0}}, {ECampaignAction::Walk, {-889, 953, 0}},
        {ECampaignAction::Walk, {-441, 1001, 0}}, {ECampaignAction::Walk, {-145, 682, 0}},
        {ECampaignAction::Use, {-145, 640, 107}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 48},
        {ECampaignAction::Walk, {-49, 569, 0}}, {ECampaignAction::Walk, {24, 568, 0}},
        {ECampaignAction::Use, {64, 568, 91}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 105},
        {ECampaignAction::Walk, {138, 568, 0}}, {ECampaignAction::Use, {138, 608, 91}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 108}, {ECampaignAction::Walk, {212, 743, 0}},
        {ECampaignAction::Use, {232, 784, 92}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 111},
        {ECampaignAction::DefeatCreature, {}, 38, TEXT("the Bad Knight guarding L74's switch hall")},
        {ECampaignAction::Walk, {227, 1015, 0}}, {ECampaignAction::Walk, {208, 1150, 0}},
        {ECampaignAction::Use, {208, 1200, 92}},
        {ECampaignAction::Ceiling, {0, 0, 152}, 104, TEXT("L74's switch raises gate 104")},
        {ECampaignAction::Frame, {208, 1200, 92}, INDEX_NONE, TEXT("city-switch")},
        {ECampaignAction::Walk, {232, 839, 0}}, {ECampaignAction::Use, {232, 800, 92}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 111}, {ECampaignAction::Walk, {144, 680, 0}},
        {ECampaignAction::Use, {144, 640, 91}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 108},
        {ECampaignAction::Walk, {136, 568, 0}}, {ECampaignAction::Use, {96, 568, 91}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 105}, {ECampaignAction::Walk, {-49, 562, 0}},
        {ECampaignAction::Walk, {-630, 484, 0}}, {ECampaignAction::Use, {-672, 484, 91}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 24}, {ECampaignAction::Walk, {-929, 457, 0}},
        {ECampaignAction::Walk, {-1250, 438, 0}}, {ECampaignAction::Use, {-1290, 438, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 187}, {ECampaignAction::Walk, {-1321, 465, 0}},
        {ECampaignAction::Walk, {-1361, 545, 0}}, {ECampaignAction::Walk, {-1345, 737, 0}},
        {ECampaignAction::Walk, {-1425, 1249, 0}}, {ECampaignAction::Walk, {-1425, 1553, 0}},
        {ECampaignAction::Walk, {-2017, 2050, 0}}, {ECampaignAction::Use, {-2040, 2081, 27}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 127}, {ECampaignAction::Walk, {-2145, 2241, 0}},
        {ECampaignAction::Walk, {-2145, 2430, 0}},
    };
    const FCampaignAction Campaign73[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2800}}, {ECampaignAction::ManaAtLeast, {0, 0, 4000}},
        {ECampaignAction::Walk, {-3625, -464, 0}}, {ECampaignAction::Walk, {-3529, -408, 0}},
        {ECampaignAction::Walk, {-3480, -283, 0}}, {ECampaignAction::Use, {-3480, -241, 180}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 240}, {ECampaignAction::Walk, {-3480, -199, 0}},
        {ECampaignAction::Walk, {-3377, -48, 0}}, {ECampaignAction::Walk, {-3225, -8, 0}},
        {ECampaignAction::Walk, {-3073, -112, 0}}, {ECampaignAction::Walk, {-3048, -243, 0}},
        {ECampaignAction::Use, {-3048, -285, 180}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 246},
        {ECampaignAction::DefeatCreature, {}, 83, TEXT("the eastern Legionary inside L73's potion room")},
        {ECampaignAction::CrossDoor, {-3048, -343, 180}, 246, TEXT("the potion-room doorway after the eastern fight"), {-3048, -243}},
        {ECampaignAction::Walk, {-3146, -450, 0}},
        {ECampaignAction::DefeatCreature, {}, 84, TEXT("the Legionary guarding L73's flight potions")},
        {ECampaignAction::Walk, {-3151, -554, 0}},
        {ECampaignAction::Walk, {-3128, -562, 0}},
        {ECampaignAction::Pickup, {-3092, -564, 0}, NativeWadRules::FlyingPotion, TEXT("an original L73 potion of flying")},
        {ECampaignAction::Walk, {-3280, -562, 0}}, {ECampaignAction::Walk, {-3280, -410, 0}},
        {ECampaignAction::Pickup, {-3280, -374, 0}, NativeWadRules::FlyingPotion, TEXT("L73's second carried flight potion")},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, TEXT("NativeWadL73PotionsReview")},
        {ECampaignAction::Rejuvenate, {0, 0, 4000}}, {ECampaignAction::ManaAtLeast, {0, 0, 4200}},
        {ECampaignAction::Walk, {-3280, -562, 0}}, {ECampaignAction::Walk, {-3128, -562, 0}},
        {ECampaignAction::Walk, {-3128, -640, 0}}, {ECampaignAction::Walk, {-3020, -640, 0}},
        {ECampaignAction::Walk, {-3020, -610, 0}},
        {ECampaignAction::Pickup, {-3020, -566, 0}, NativeWadRules::FlyingPotion, TEXT("L73's third original flight potion beyond the chair")},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, TEXT("NativeWadL73AllPotionsReview")},
        {ECampaignAction::Walk, {-3020, -640, 0}}, {ECampaignAction::Walk, {-3128, -640, 0}},
        {ECampaignAction::Walk, {-3128, -562, 0}},
        {ECampaignAction::Walk, {-3048, -343, 0}}, {ECampaignAction::Use, {-3048, -301, 180}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 246},
        {ECampaignAction::Walk, {-3048, -242, 0}}, {ECampaignAction::Walk, {-3073, -112, 0}},
        {ECampaignAction::Walk, {-3225, -8, 0}},
        {ECampaignAction::Walk, {-3257, 316, 0}}, {ECampaignAction::Use, {-3257, 358, 179}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 226}, {ECampaignAction::Walk, {-3268, 618, 0}},
        {ECampaignAction::WaitBoulder, {-3180, -3050, 0}, 223},
        {ECampaignAction::Use, {-3268, 660, 91}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 37},
        {ECampaignAction::Walk, {-3268, 777, 0}}, {ECampaignAction::Use, {-3268, 816, 75}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 249},
        {ECampaignAction::Walk, {-3273, 1272, 0}},
        {ECampaignAction::Walk, {-3353, 1344, 0}}, {ECampaignAction::Walk, {-3734, 1346, 0}},
        {ECampaignAction::Use, {-3776, 1346, 3}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 150},
        {ECampaignAction::Walk, {-3957, 1347, 0}}, {ECampaignAction::Use, {-3999, 1347, 83}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 119}, {ECampaignAction::Walk, {-4217, 1376, 0}},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {-4382, 1542, 151}}, {ECampaignAction::Fly, {-4363, 1590, 151}},
        {ECampaignAction::Ceiling, {0, 0, 175}, 118},
        {ECampaignAction::Fly, {-4562, 1304, 151}}, {ECampaignAction::Fly, {-4610, 1329, 151}},
        {ECampaignAction::Ceiling, {0, 0, 195}, 118},
        {ECampaignAction::Fly, {-4505, 1318, 151}}, {ECampaignAction::Fly, {-4405, 1220, 151}, 16},
        {ECampaignAction::Fly, {-4321, 1125, 151}}, {ECampaignAction::Fly, {-4345, 1078, 151}},
        {ECampaignAction::Ceiling, {0, 0, 232}, 118, TEXT("L73's three plates raise gate 118 through all three heights")},
        {ECampaignAction::Ceiling, {0, 0, 232}, 136},
        {ECampaignAction::CrossDoor, {-3953, 1344, 83}, 119, TEXT("the plate-room return door"), {-4056, 1347}},
        {ECampaignAction::Walk, {-3825, 1346, 0}},
        {ECampaignAction::CrossDoor, {-3728, 1346, 19}, 150, TEXT("the plate-wing return door"), {-3825, 1346}},
        {ECampaignAction::Walk, {-3329, 1328, 0}},
        {ECampaignAction::Walk, {-3260, 866, 0}},
        {ECampaignAction::WaitBoulder, {-3180, -3050, 0}, 223, TEXT("L73's boulder clears the return route")},
        {ECampaignAction::CrossDoor, {-3260, 776, 59}, 249, TEXT("the return door beyond L73's boulder"), {-3260, 866}},
        {ECampaignAction::Walk, {-2729, 784, 0}},
        {ECampaignAction::Walk, {-2617, 880, 0}}, {ECampaignAction::Walk, {-1969, 920, 0}},
        {ECampaignAction::Walk, {-1969, 976, 0}}, {ECampaignAction::Walk, {-2025, 1086, 0}},
        {ECampaignAction::CrossDoor, {-1944, 1136, 123}, 18, TEXT("L73's recovery-wing approach door"), {-2048, 1136}},
        {ECampaignAction::Walk, {-1129, 1176, 0}},
        {ECampaignAction::Teleport, {-1040, 1264, 0}, INDEX_NONE, TEXT("L73's source teleporter reaches the recovery wing"), {-619, 1147}},
        {ECampaignAction::FlightEnded},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, TEXT("NativeWadL73RecoveryReview")},
        {ECampaignAction::Walk, {-458, 1168, 0}}, {ECampaignAction::Use, {-416, 1168, 36}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 95},
        {ECampaignAction::Walk, {-377, 1168, 0}}, {ECampaignAction::Walk, {-226, 1318, 0}},
        {ECampaignAction::HealthAtLeast, {0, 0, 1800}, INDEX_NONE, TEXT("L73's source recovery sector restores health")},
        {ECampaignAction::Walk, {-374, 1496, 0}},
        {ECampaignAction::CrossDoor, {-454, 1496, 51}, 209, TEXT("L73's recovery-room exit"), {-374, 1496}}, {ECampaignAction::Walk, {-1441, 1592, 0}},
        {ECampaignAction::Walk, {-1735, 1608, 0}},
        {ECampaignAction::CrossDoor, {-1816, 1608, 148}, 214, TEXT("L73's outer return door"), {-1735, 1608}}, {ECampaignAction::Walk, {-1865, 1728, 0}},
        {ECampaignAction::Walk, {-1832, 2184, 0}},
        {ECampaignAction::CrossDoor, {-1738, 2184, 148}, 206, TEXT("L73's northeast return door"), {-1832, 2184}},
        {ECampaignAction::Walk, {-1137, 2174, 0}},
        {ECampaignAction::CrossDoor, {-1137, 2284, 148}, 207, TEXT("L73's final return door"), {-1137, 2174}},
        {ECampaignAction::Walk, {-1041, 2496, 0}},
        {ECampaignAction::Walk, {-1029, 2864, 0}},
    };
    const FCampaignAction Campaign72[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3000}}, {ECampaignAction::ManaAtLeast, {0, 0, 4200}},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {2304, 366, 40}}, {ECampaignAction::Fly, {2202, 366, 40}},
        {ECampaignAction::Use, {2160, 366, 40}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 228},
        {ECampaignAction::Fly, {2072, 339, 40}}, {ECampaignAction::Fly, {1618, -106, 40}},
        {ECampaignAction::Use, {1579, -106, 40}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 156},
        {ECampaignAction::Fly, {1516, -106, 40}}, {ECampaignAction::Fly, {1496, 43, 40}},
        {ECampaignAction::Fly, {1449, 773, 40}}, {ECampaignAction::Use, {1406, 773, 40}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 158}, {ECampaignAction::Fly, {1376, 803, 40}},
        {ECampaignAction::Fly, {1384, 899, 140}}, {ECampaignAction::Fly, {1460, 946, 140}},
        {ECampaignAction::Use, {1485, 946, 170}},
        {ECampaignAction::Floor, {0, 0, -56}, 295, TEXT("L72's wall lowers the hidden floor")},
        {ECampaignAction::Fly, {872, 842, 140}}, {ECampaignAction::Fly, {464, 627, 120}},
        {ECampaignAction::Fly, {128, 624, 160}}, {ECampaignAction::Fly, {-128, 624, 160}},
        {ECampaignAction::Fly, {-370, 624, 190}},
        {ECampaignAction::Use, {-412, 624, 200}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 37}, {ECampaignAction::Fly, {-552, 627, 200}},
        {ECampaignAction::Fly, {-1472, 1075, 200}}, {ECampaignAction::Fly, {-1592, 1061, 200}},
        {ECampaignAction::Teleport, {-1592, 1020, 0}, INDEX_NONE, TEXT("L72's portal reaches the Wand chamber"), {-3328, -219}},
        {ECampaignAction::Fly, {-3392, 942, 40}},
        {ECampaignAction::Pickup, {-3392, 984, 12}, NativeWadRules::WandOfDisintegration, TEXT("the original L72 Wand of Disintegration pickup")},
        {ECampaignAction::Fly, {-3392, 400, 40}},
        {ECampaignAction::FireWand, {}, NativeWadRules::WandOfDisintegration},
        {ECampaignAction::Fly, {-3312, 934, 140}}, {ECampaignAction::Fly, {-3312, 994, 140}},
        {ECampaignAction::Fly, {-3312, 1045, 200}},
        {ECampaignAction::Fly, {-3312, 1071, 265}},
        {ECampaignAction::Fly, {-3312, 1136, 265}},
    };
    const FCampaignAction Campaign32[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3200}},
        {ECampaignAction::Walk, {120, 3089, 0}}, {ECampaignAction::Use, {120, 3049, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 133}, {ECampaignAction::Walk, {120, 2396, 0}},
        {ECampaignAction::Walk, {8, 2280, 0}}, {ECampaignAction::Use, {8, 2242, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 141}, {ECampaignAction::Walk, {8, 2188, 0}},
        {ECampaignAction::Walk, {-48, 2044, 0}}, {ECampaignAction::Walk, {-200, 1972, 0}},
        {ECampaignAction::Walk, {-224, 1932, 0}}, {ECampaignAction::Walk, {-208, 1820, 0}},
        {ECampaignAction::Walk, {-176, 1788, 0}}, {ECampaignAction::Walk, {132, 1788, 0}},
        {ECampaignAction::Use, {172, 1788, 235}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 218},
        {ECampaignAction::Walk, {248, 1732, 0}}, {ECampaignAction::Walk, {368, 1596, 0}},
        {ECampaignAction::Walk, {360, 1540, 0}}, {ECampaignAction::Walk, {272, 1492, 0}},
        {ECampaignAction::Walk, {-37, 1465, 0}}, {ECampaignAction::Use, {-37, 1425, 163}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 206}, {ECampaignAction::Walk, {-36, 1404, 0}},
        {ECampaignAction::Walk, {56, 1332, 0}}, {ECampaignAction::Walk, {689, 1382, 0}},
        {ECampaignAction::Use, {689, 1424, 42}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 207},
        {ECampaignAction::Walk, {696, 1465, 0}}, {ECampaignAction::Walk, {744, 1512, 0}},
        {ECampaignAction::Use, {775, 1512, 59}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 208},
        {ECampaignAction::Walk, {944, 1468, 0}}, {ECampaignAction::Walk, {1016, 1372, 0}},
        {ECampaignAction::Walk, {1000, 1073, 0}}, {ECampaignAction::Use, {1000, 1032, 195}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 216}, {ECampaignAction::Walk, {1000, 983, 0}},
        {ECampaignAction::Walk, {936, 932, 0}}, {ECampaignAction::Walk, {832, 924, 0}},
        {ECampaignAction::Walk, {706, 836, 0}}, {ECampaignAction::Use, {656, 836, 196}},
        {ECampaignAction::Ceiling, {0, 0, 328}, 108}, {ECampaignAction::Ceiling, {0, 0, 280}, 233,
            TEXT("L32's delayed switch opens both gates")},
        {ECampaignAction::Walk, {993, 975, 0}}, {ECampaignAction::Use, {993, 1016, 195}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 216}, {ECampaignAction::Walk, {1039, 1285, 0}},
        {ECampaignAction::Walk, {1080, 1486, 0}}, {ECampaignAction::Use, {1120, 1492, 75}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 52}, {ECampaignAction::Walk, {1294, 1492, 0}},
        {ECampaignAction::Use, {1336, 1492, 75}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 220},
        {ECampaignAction::Walk, {1805, 1536, 0}}, {ECampaignAction::Use, {1845, 1536, 11}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 127}, {ECampaignAction::Walk, {1950, 1535, 0}},
    };
    const FCampaignAction Campaign71[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3400}},
        {ECampaignAction::Walk, {-1240, -1300, 0}}, {ECampaignAction::Walk, {-1240, -1342, 0}},
        {ECampaignAction::Use, {-1280, -1342, -77}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 73},
        {ECampaignAction::Walk, {-1353, -1342, 0}}, {ECampaignAction::Walk, {-1808, -1296, 0}},
        {ECampaignAction::Walk, {-2120, -704, 0}}, {ECampaignAction::Walk, {-2168, -88, 0}},
        {ECampaignAction::Walk, {-1968, 528, 0}}, {ECampaignAction::Walk, {-1712, 1008, 0}},
        {ECampaignAction::Walk, {-1480, 2024, 0}}, {ECampaignAction::Walk, {-1392, 2320, 0}},
        {ECampaignAction::Walk, {-808, 2464, 0}}, {ECampaignAction::Walk, {-528, 2408, 0}},
        {ECampaignAction::Use, {-528, 2368, 23}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 59},
        {ECampaignAction::Walk, {-528, 2296, 0}}, {ECampaignAction::Walk, {-392, 1832, 0}},
        {ECampaignAction::Walk, {-142, 1640, 0}}, {ECampaignAction::Use, {-142, 1600, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 27}, {ECampaignAction::Walk, {-184, 1360, 0}},
        {ECampaignAction::Walk, {-592, 1128, 0}}, {ECampaignAction::Walk, {-552, 880, 0}},
        {ECampaignAction::Walk, {272, 472, 0}}, {ECampaignAction::Walk, {442, 153, 0}},
        {ECampaignAction::Jump, {442, 44, 0}, 112}, {ECampaignAction::Walk, {224, -42, 0}},
        {ECampaignAction::Walk, {-483, 58, 0}}, {ECampaignAction::Walk, {-746, -64, 0}},
        {ECampaignAction::Floor, {0, 0, 200}, 84, TEXT("L71's entry strip raises platform 84 to Z200")},
        {ECampaignAction::Walk, {-489, -73, 0}}, {ECampaignAction::Use, {-464, -73, 48}},
        {ECampaignAction::Floor, {0, 0, 16}, 84, TEXT("L71's switch lowers platform 84 to Z16")},
        {ECampaignAction::Floor, {0, 0, 200}, 84, TEXT("L71's platform returns to Z200 after the delay")},
        {ECampaignAction::Walk, {-491, 146, 0}}, {ECampaignAction::Walk, {56, 824, 0}},
        {ECampaignAction::Use, {56, 866, -77}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 109},
        {ECampaignAction::Walk, {56, 944, 0}}, {ECampaignAction::Walk, {232, 1224, 0}},
        {ECampaignAction::Walk, {628, 1225, 0}}, {ECampaignAction::Use, {670, 1225, 83}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 101}, {ECampaignAction::Walk, {800, 1296, 0}},
        {ECampaignAction::Walk, {1464, 1768, 0}}, {ECampaignAction::Walk, {2448, 2640, 0}},
        {ECampaignAction::Walk, {2416, 3256, 0}}, {ECampaignAction::Walk, {1992, 3496, 0}},
        {ECampaignAction::Walk, {1610, 3500, 0}}, {ECampaignAction::Walk, {944, 3904, 0}},
        {ECampaignAction::Walk, {456, 4260, 0}},
    };
    const FCampaignAction Campaign70[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3400}},
        {ECampaignAction::Walk, {-1346, 496, 0}}, {ECampaignAction::Use, {-1304, 496, -165}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 19}, {ECampaignAction::Walk, {-472, 497, 0}},
        {ECampaignAction::Use, {-432, 497, -165}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 25},
        {ECampaignAction::Walk, {-352, 464, 0}},
        {ECampaignAction::Walk, {-240, 320, 0}}, {ECampaignAction::Walk, {-208, 184, 0}},
        {ECampaignAction::Walk, {48, 136, 0}}, {ECampaignAction::Walk, {168, 176, 0}},
        {ECampaignAction::Walk, {360, 128, 0}}, {ECampaignAction::Walk, {584, 176, 0}},
        {ECampaignAction::Walk, {608, 224, 0}}, {ECampaignAction::TouchSector, {611, 476, 0}, 36},
        {ECampaignAction::Floor, {0, 0, 0}, 36, TEXT("L70's pressure lift reaches the upper hall")},
        {ECampaignAction::Walk, {655, 496, 0}}, {ECampaignAction::Use, {672, 496, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 35}, {ECampaignAction::TouchSector, {1270, 496, 0}, 1},
        {ECampaignAction::Ceiling, {0, 0, -88}, 53, TEXT("L70's upper hall opens the lower passage")},
        {ECampaignAction::Walk, {730, 496, 0}}, {ECampaignAction::Use, {688, 496, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 35}, {ECampaignAction::Walk, {610, 474, 0}},
        {ECampaignAction::Walk, {608, 416, 0}}, {ECampaignAction::Walk, {596, 184, 0}},
        {ECampaignAction::Walk, {544, 160, 0}}, {ECampaignAction::Walk, {240, 136, 0}},
        {ECampaignAction::Walk, {120, 176, 0}}, {ECampaignAction::Walk, {-72, 128, 0}},
        {ECampaignAction::Walk, {-200, 176, 0}}, {ECampaignAction::Walk, {-232, 240, 0}},
        {ECampaignAction::Walk, {-200, 392, 0}}, {ECampaignAction::Walk, {-92, 495, 0}},
        // Approach from below the switch: the direct diagonal clips its raised pedestal at (384, 592).
        {ECampaignAction::Walk, {218, 495, 0}}, {ECampaignAction::Walk, {330, 640, 0}},
        {ECampaignAction::Walk, {400, 640, 0}}, {ECampaignAction::Walk, {400, 617, 0}},
        {ECampaignAction::Use, {400, 592, -146}},
        {ECampaignAction::Ceiling, {0, 0, 190}, 56, TEXT("L70's switch raises gate 56")},
        {ECampaignAction::Walk, {319, 580, 0}}, {ECampaignAction::Jump, {273, 520, 0}, 65},
        {ECampaignAction::Walk, {-144, 448, 0}}, {ECampaignAction::Walk, {-232, 320, 0}},
        {ECampaignAction::Walk, {-208, 184, 0}}, {ECampaignAction::Walk, {-160, 160, 0}},
        {ECampaignAction::Walk, {168, 176, 0}}, {ECampaignAction::Walk, {360, 128, 0}},
        {ECampaignAction::Walk, {584, 176, 0}}, {ECampaignAction::Walk, {608, 224, 0}},
        {ECampaignAction::TouchSector, {611, 476, 0}, 36}, {ECampaignAction::Floor, {0, 0, 0}, 36},
        {ECampaignAction::Walk, {655, 496, 0}}, {ECampaignAction::Use, {672, 496, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 35}, {ECampaignAction::Walk, {1696, 496, 0}},
        {ECampaignAction::Walk, {1976, 486, 0}},
    };
    const FCampaignAction Campaign61[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3200}},
        {ECampaignAction::Walk, {1490, 2720, 0}}, {ECampaignAction::Walk, {1397, 2720, 0}},
        {ECampaignAction::Jump, {1362, 2720, 0}, 40},
        {ECampaignAction::Walk, {1368, 2838, 0}}, {ECampaignAction::Walk, {1328, 3028, 0}},
        {ECampaignAction::Walk, {1160, 3108, 0}}, {ECampaignAction::Walk, {656, 3132, 0}},
        {ECampaignAction::Walk, {440, 3156, 0}}, {ECampaignAction::Walk, {86, 2275, 0}},
        {ECampaignAction::Walk, {-18, 2275, 0}}, {ECampaignAction::Walk, {-328, 3154, 0}},
        {ECampaignAction::Jump, {-416, 3160, 0}, 61},
        {ECampaignAction::Walk, {-712, 2868, 0}}, {ECampaignAction::Walk, {-776, 2868, 0}},
        {ECampaignAction::Walk, {-832, 2922, 0}}, {ECampaignAction::Walk, {-832, 2982, 0}},
        {ECampaignAction::Ceiling, {0, 0, 380}, 143}, {ECampaignAction::Ceiling, {0, 0, 264}, 62,
            TEXT("L61's pressure plate opens both ceilings")},
        {ECampaignAction::Walk, {-768, 2852, 0}}, {ECampaignAction::Walk, {-215, 2189, 0}},
        {ECampaignAction::Walk, {-155, 2189, 0}}, {ECampaignAction::Jump, {-96, 2189, 0}, 60},
        // The middle ledge is -40 and the far bank is 0: land, then jump the second lip.
        {ECampaignAction::Jump, {-36, 2189, 0}, 45},
        {ECampaignAction::Walk, {64, 1976, 0}}, {ECampaignAction::Walk, {-960, 1976, 0}},
        {ECampaignAction::Walk, {-1976, 1776, 0}}, {ECampaignAction::Walk, {-2200, 1776, 0}},
        {ECampaignAction::Walk, {-2215, 1624, 0}},
        {ECampaignAction::Walk, {-2220, 1513, 0}}, {ECampaignAction::Use, {-2220, 1472, 51}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 88}, {ECampaignAction::Walk, {-2214, 1390, 0}},
        {ECampaignAction::Walk, {-2088, 828, 0}}, {ECampaignAction::Walk, {-2090, 766, 0}},
        {ECampaignAction::Use, {-2048, 766, 91}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 96},
        {ECampaignAction::Walk, {-1920, 644, 0}}, {ECampaignAction::Walk, {-1900, 254, 0}},
        {ECampaignAction::Use, {-1860, 254, 147}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 101},
        {ECampaignAction::Walk, {-1736, 170, 0}}, {ECampaignAction::Walk, {-1624, -158, 0}},
        {ECampaignAction::Use, {-1624, -200, 179}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 107},
        {ECampaignAction::Walk, {-1588, -712, 0}},
        {ECampaignAction::Pickup, {-1628, -672, 148}, NativeWadRules::ElmoreKey8, TEXT("the original L61 Key 8")},
        {ECampaignAction::Walk, {-1624, -308, 0}}, {ECampaignAction::Use, {-1624, -264, 179}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 107}, {ECampaignAction::Walk, {-1632, 28, 0}},
        {ECampaignAction::Walk, {-1750, 248, 0}}, {ECampaignAction::Use, {-1792, 248, 147}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 101}, {ECampaignAction::Walk, {-1920, 356, 0}},
        {ECampaignAction::Walk, {-1943, 759, 0}}, {ECampaignAction::Use, {-1984, 759, 91}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 96}, {ECampaignAction::Walk, {-2120, 740, 0}},
        {ECampaignAction::Walk, {-2170, 744, 0}}, {ECampaignAction::Use, {-2170, 704, 91}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 95}, {ECampaignAction::Walk, {-2219, 598, 0}},
        {ECampaignAction::Walk, {-2416, 452, 0}}, {ECampaignAction::Walk, {-2855, 448, 0}},
    };
    const FCampaignAction Campaign40[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3400}},
        {ECampaignAction::Walk, {32, -3650, 0}}, {ECampaignAction::Walk, {32, -2634, 0}},
        {ECampaignAction::Walk, {-4, -2288, 0}},
        {ECampaignAction::TouchSector, {-64, -2289, 0}, 51},
        {ECampaignAction::Floor, {0, 0, 690}, 51, TEXT("L40's west lift reaches the upper platform")},
        {ECampaignAction::Walk, {-164, -2306, 0}}, {ECampaignAction::Walk, {-331, -2314, 0}},
        {ECampaignAction::Floor, {0, 0, 840}, 27}, {ECampaignAction::Walk, {-372, -2314, 0}},
        {ECampaignAction::Walk, {-372, -1860, 0}},
        {ECampaignAction::Walk, {-236, -1808, 0}}, {ECampaignAction::Use, {-186, -1808, 884}},
        {ECampaignAction::Walk, {-372, -1860, 0}}, {ECampaignAction::Walk, {-372, -2314, 0}},
        {ECampaignAction::Walk, {-154, -2294, 0}}, {ECampaignAction::Walk, {-64, -2289, 0}},
        {ECampaignAction::Walk, {64, -2288, 0}},
        {ECampaignAction::TouchSector, {128, -2289, 0}, 10},
        {ECampaignAction::Floor, {0, 0, 690}, 10, TEXT("L40's east lift reaches the upper platform")},
        {ECampaignAction::Walk, {227, -2306, 0}}, {ECampaignAction::Walk, {364, -2314, 0}},
        {ECampaignAction::Floor, {0, 0, 840}, 26}, {ECampaignAction::Walk, {404, -2314, 0}},
        {ECampaignAction::Walk, {404, -1858, 0}},
        {ECampaignAction::Walk, {302, -1808, 0}}, {ECampaignAction::Use, {252, -1808, 884}},
        {ECampaignAction::Ceiling, {0, 0, 444}, 41, TEXT("L40's two switches open gate 41")},
        {ECampaignAction::Walk, {232, -1874, 0}}, {ECampaignAction::Walk, {120, -1850, 0}},
        {ECampaignAction::Walk, {32, -1754, 0}}, {ECampaignAction::Walk, {32, -1594, 0}},
        {ECampaignAction::Walk, {32, -690, 0}}, {ECampaignAction::Walk, {0, -340, 0}},
        {ECampaignAction::Jump, {-24, -270, 0}, 65}, {ECampaignAction::Walk, {-24, -128, 0}},
        {ECampaignAction::Jump, {-24, -64, 0}, 60}, {ECampaignAction::Walk, {-24, 27, 0}},
        {ECampaignAction::Jump, {-24, 102, 0}, 67}, {ECampaignAction::Walk, {-24, 125, 0}},
        {ECampaignAction::Jump, {-24, 199, 0}, 65},
        {ECampaignAction::Walk, {-31, 261, 0}},
        {ECampaignAction::Floor, {0, 0, 700}, 6, TEXT("L40's main lift reaches the flight supplies")},
        {ECampaignAction::Walk, {-41, 336, 0}},
        {ECampaignAction::Pickup, {-56, 372, 715}, NativeWadRules::FlyingPotion, TEXT("the original L40 potion of flying")},
        {ECampaignAction::Pickup, {-16, 347, 714}, NativeWadRules::FlyingAmulet, TEXT("the original L40 amulet of flying")},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {-31, 256, 790}}, {ECampaignAction::Fly, {-32, 128, 790}},
        {ECampaignAction::Fly, {160, 128, 650}}, {ECampaignAction::Fly, {160, 740, 520}},
        {ECampaignAction::Fly, {-64, 1810, 490}},
        {ECampaignAction::Fly, {-64, 2028, 450}, INDEX_NONE, TEXT("L40's potion carries the player over the raised gate ledge")},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingAmulet},
        {ECampaignAction::Fly, {-576, 2510, 445}}, {ECampaignAction::Fly, {-1235, 2990, 445}},
        {ECampaignAction::Fly, {-1336, 3142, 445}}, {ECampaignAction::Fly, {-1336, 3444, 431}},
        {ECampaignAction::Fly, {-1224, 3590, 431}}, {ECampaignAction::Fly, {-208, 3686, 412}},
        {ECampaignAction::Fly, {-56, 3714, 412}}, {ECampaignAction::Fly, {-56, 3714, 35}},
        {ECampaignAction::Fly, {808, 3878, 35}}, {ECampaignAction::Fly, {1072, 3822, 35}},
        {ECampaignAction::Fly, {1368, 3350, 35}}, {ECampaignAction::Fly, {1430, 3322, 285}},
        {ECampaignAction::Fly, {1880, 3126, 285}}, {ECampaignAction::Fly, {2267, 3023, 234}},
        {ECampaignAction::Frame, {2304, 3008, 220}, INDEX_NONE, TEXT("orb-island")},
        {ECampaignAction::Pickup, {2304, 3008, 212}, NativeWadRules::OrbOfDestruction, TEXT("the original L40 Orb of Destruction pickup")},
        {ECampaignAction::Fly, {2376, 3040, 234}}, {ECampaignAction::Pickup, {2336, 3040, 212}, NativeWadRules::Key4},
        {ECampaignAction::Fly, {1880, 3126, 285}}, {ECampaignAction::Fly, {1430, 3322, 285}},
        {ECampaignAction::Fly, {1368, 3350, 35}}, {ECampaignAction::Fly, {1072, 3822, 35}},
        {ECampaignAction::Fly, {808, 3878, 35}}, {ECampaignAction::Fly, {-56, 3714, 35}},
        {ECampaignAction::Fly, {-56, 3714, 420}}, {ECampaignAction::Fly, {-208, 3686, 420}},
        {ECampaignAction::Fly, {-1224, 3590, 431}}, {ECampaignAction::Fly, {-1336, 3444, 431}},
        {ECampaignAction::Fly, {-1336, 3142, 445}}, {ECampaignAction::Fly, {-1235, 2990, 445}},
        {ECampaignAction::Fly, {-576, 2510, 445}}, {ECampaignAction::Fly, {456, 2502, 443}},
        {ECampaignAction::Fly, {496, 2582, 443}}, {ECampaignAction::Use, {515, 2624, 465}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 123, TEXT("L40 Key 4 opens door 123")},
        {ECampaignAction::Fly, {552, 2646, 443}}, {ECampaignAction::Fly, {680, 2630, 433}},
        {ECampaignAction::Fly, {752, 2558, 423}}, {ECampaignAction::Use, {752, 2512, 445}},
        {ECampaignAction::Floor, {0, 0, 430}, 128, TEXT("L40's last switch lowers the platforms and opens the exit route")},
        {ECampaignAction::Fly, {680, 2630, 441}}, {ECampaignAction::Fly, {556, 2646, 441}},
        {ECampaignAction::Use, {526, 2624, 465}}, {ECampaignAction::OpenDoor, {0, 0, 56}, 123},
        {ECampaignAction::Fly, {456, 2502, 443}}, {ECampaignAction::Fly, {0, 2526, 441}},
        {ECampaignAction::Fly, {-184, 2894, 439}}, {ECampaignAction::Fly, {-122, 3296, 439}},
    };
    const FCampaignAction Campaign64[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3350}},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, TEXT("NativeWadCampaignL64RestedReview")},
        {ECampaignAction::Walk, {971, 2657, 0}}, {ECampaignAction::Walk, {211, 2569, 0}},
        {ECampaignAction::Walk, {19, 2337, 0}}, {ECampaignAction::Walk, {28, 2192, 0}},
        {ECampaignAction::Use, {28, 2160, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 255},
        {ECampaignAction::Walk, {24, 2108, 0}}, {ECampaignAction::Walk, {32, 1392, 0}},
        {ECampaignAction::Use, {32, 1360, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 155},
        {ECampaignAction::Walk, {35, 1321, 0}}, {ECampaignAction::Walk, {51, 889, 0}},
        {ECampaignAction::Walk, {91, 865, 0}},
        {ECampaignAction::Walk, {571, 849, 0}}, {ECampaignAction::Walk, {1043, 337, 0}},
        {ECampaignAction::Walk, {1299, 161, 0}}, {ECampaignAction::Walk, {1267, -247, 0}},
        {ECampaignAction::Walk, {1131, -319, 0}}, {ECampaignAction::Walk, {299, -391, 0}},
        {ECampaignAction::Walk, {59, -367, 0}}, {ECampaignAction::Walk, {27, -335, 0}},
        {ECampaignAction::Walk, {19, 249, 0}}, {ECampaignAction::Use, {18, 280, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 38}, {ECampaignAction::Walk, {3, 305, 0}},
        {ECampaignAction::Walk, {-43, 354, 0}},
        {ECampaignAction::Use, {-77, 356, 45}}, {ECampaignAction::OpenDoor, {0, 0, 75}, 279},
        {ECampaignAction::Walk, {-117, 353, 0}}, {ECampaignAction::Walk, {-349, 337, 0}},
        {ECampaignAction::Walk, {-357, 329, 0}}, {ECampaignAction::Walk, {-469, 145, 0}},
        // Leave room to pass south of the barbarian at (-512, 128).
        {ECampaignAction::Walk, {-449, 75, 0}}, {ECampaignAction::Walk, {-630, 73, 0}},
        {ECampaignAction::Walk, {-765, 105, 0}}, {ECampaignAction::Walk, {-907, 108, 0}},
    };
    const FCampaignAction Campaign65[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3300}},
        {ECampaignAction::Walk, {-800, 2232, 0}}, {ECampaignAction::Walk, {-792, 2192, 0}},
        {ECampaignAction::Use, {-792, 2160, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 181},
        {ECampaignAction::Walk, {-790, 2124, 0}}, {ECampaignAction::Walk, {-752, 2112, 0}},
        {ECampaignAction::Walk, {-416, 1912, 0}}, {ECampaignAction::Walk, {-216, 1728, 0}},
        {ECampaignAction::Walk, {800, 1696, 0}}, {ECampaignAction::Walk, {1104, 1744, 0}},
        {ECampaignAction::Walk, {1304, 2076, 0}}, {ECampaignAction::Use, {1314, 2112, 144}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 38}, {ECampaignAction::Walk, {1318, 2186, 0}},
        {ECampaignAction::Walk, {1320, 2210, 0}}, {ECampaignAction::Use, {1280, 2216, 144}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 112}, {ECampaignAction::Walk, {1216, 2255, 0}},
        {ECampaignAction::Walk, {1221, 2360, 0}}, {ECampaignAction::Use, {1221, 2400, 144}},
        {ECampaignAction::Walk, {1120, 2310, 0}},
        {ECampaignAction::Teleport, {1120, 2373, 0}, INDEX_NONE, TEXT("L65's first teleporter reaches its authored destination"), {1628, 2370}},
        {ECampaignAction::Walk, {1470, 2368, 0}}, {ECampaignAction::Use, {1466, 2400, 144}},
        {ECampaignAction::Teleport, {1628, 2370, 0}, INDEX_NONE, TEXT("L65's second teleporter reaches the exit lift area"), {2234, 1600}},
        // Go around sector 117, which teleports back to the first pad.
        {ECampaignAction::Walk, {2234, 1672, 0}}, {ECampaignAction::Walk, {2460, 1662, 0}},
        {ECampaignAction::Walk, {2624, 1619, 0}}, {ECampaignAction::Use, {2656, 1619, 233}},
        {ECampaignAction::OpenDoor, {0, 0, 58}, 154}, {ECampaignAction::Walk, {2706, 1619, 0}},
        {ECampaignAction::Floor, {0, 0, 0}, 153, TEXT("L65's original lift lowers to the exit floor")},
        {ECampaignAction::Use, {2733, 1619, 42}}, {ECampaignAction::OpenDoor, {0, 0, 58}, 172},
        {ECampaignAction::Walk, {2852, 1619, 0}},
    };
    const FCampaignAction Campaign34[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2100}}, {ECampaignAction::ManaAtLeast, {0, 0, 3400}},
        {ECampaignAction::Walk, {914, 1273, 0}}, {ECampaignAction::Walk, {306, 1233, 0}},
        {ECampaignAction::Walk, {106, 921, 0}}, {ECampaignAction::Walk, {114, 745, 0}},
        {ECampaignAction::Walk, {282, 585, 0}}, {ECampaignAction::Walk, {288, 448, 0}},
        {ECampaignAction::Walk, {288, 359, 0}},
        {ECampaignAction::Walk, {202, 305, 0}}, {ECampaignAction::Walk, {170, 321, 0}},
        {ECampaignAction::Walk, {116, 385, 0}},
        {ECampaignAction::Walk, {-174, 657, 0}}, {ECampaignAction::Walk, {-214, 657, 0}},
        {ECampaignAction::Walk, {-350, 502, 0}}, {ECampaignAction::Use, {-384, 490, 40}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 12}, {ECampaignAction::Walk, {-454, 497, 0}},
        {ECampaignAction::Walk, {-462, 505, 0}}, {ECampaignAction::Walk, {-950, 1521, 0}},
        {ECampaignAction::Walk, {-982, 1681, 0}}, {ECampaignAction::Walk, {-1062, 1786, 0}},
        {ECampaignAction::Use, {-1101, 1786, 78}}, {ECampaignAction::OpenDoor, {0, 0, 94}, 107},
        {ECampaignAction::Walk, {-1134, 1817, 0}}, {ECampaignAction::Walk, {-1126, 1889, 0}},
        {ECampaignAction::Walk, {-1014, 1977, 0}}, {ECampaignAction::Walk, {-846, 1953, 0}},
        {ECampaignAction::Walk, {-742, 1801, 0}}, {ECampaignAction::Walk, {-718, 1801, 0}},
        {ECampaignAction::Walk, {-702, 1833, 0}}, {ECampaignAction::Walk, {-726, 1961, 0}},
        {ECampaignAction::Walk, {-902, 2073, 0}}, {ECampaignAction::Walk, {-1305, 2035, 0}},
        {ECampaignAction::Pickup, {-1334, 2031, 772}, 258, TEXT("the lighthouse stairway key")},
        {ECampaignAction::Walk, {-846, 2065, 0}}, {ECampaignAction::Walk, {-702, 1913, 0}},
        {ECampaignAction::Walk, {-710, 1809, 0}}, {ECampaignAction::Walk, {-742, 1801, 0}},
        {ECampaignAction::Walk, {-862, 1961, 0}}, {ECampaignAction::Walk, {-1062, 1961, 0}},
        {ECampaignAction::Walk, {-1134, 1841, 0}}, {ECampaignAction::Walk, {-1157, 1786, 0}},
        {ECampaignAction::Use, {-1119, 1786, 93}}, {ECampaignAction::OpenDoor, {0, 0, 94}, 107},
        {ECampaignAction::Walk, {-1062, 1786, 0}}, {ECampaignAction::Walk, {-982, 1681, 0}},
        {ECampaignAction::Walk, {-950, 1654, 0}}, {ECampaignAction::Walk, {-950, 1517, 0}},
        {ECampaignAction::Walk, {-558, 601, 0}},
        {ECampaignAction::Walk, {-462, 505, 0}}, {ECampaignAction::Walk, {-428, 490, 0}},
        {ECampaignAction::Use, {-392, 490, 40}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 12},
        {ECampaignAction::Walk, {-358, 497, 0}}, {ECampaignAction::Walk, {-206, 657, 0}},
        {ECampaignAction::Walk, {-96, 675, 0}}, {ECampaignAction::Walk, {-96, 486, 0}},
        {ECampaignAction::Walk, {78, 478, 0}},
        {ECampaignAction::Walk, {143, 520, 0}}, {ECampaignAction::Use, {161, 544, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 155}, {ECampaignAction::Walk, {197, 580, 0}},
        {ECampaignAction::Teleport, {210, 601, 0}, INDEX_NONE, TEXT("L34's first teleporter reaches the mountain passage"), {-1012, -1572}},
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3400}},
        {ECampaignAction::Walk, {-989, -1572, 0}}, {ECampaignAction::Use, {-951, -1572, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 135}, {ECampaignAction::Walk, {-884, -1572, 0}},
        {ECampaignAction::Walk, {-884, -1434, 0}},
        {ECampaignAction::Jump, {-532, -1434, 0}, 148, TEXT("the player jumps over L34's lowered passage")},
        {ECampaignAction::Walk, {-311, -1598, 0}}, {ECampaignAction::Walk, {-307, -1890, 0}},
        {ECampaignAction::Walk, {-795, -1896, 0}}, {ECampaignAction::Use, {-795, -1948, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 134}, {ECampaignAction::Walk, {-795, -2168, 0}},
        // Trigger the crusher, retreat, and cross during its reset.
        {ECampaignAction::Walk, {-795, -2232, 0}}, {ECampaignAction::Walk, {-795, -2168, 0}},
        {ECampaignAction::CeilingAtMost, {0, 0, 90}, 160, TEXT("L34's crusher is in its closing cycle")},
        {ECampaignAction::OpenDoor, {0, 0, 64}, 160},
        {ECampaignAction::Walk, {-795, -2406, 0}}, {ECampaignAction::Walk, {-795, -2560, 0}},
        {ECampaignAction::Use, {-795, -2605, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 163},
        {ECampaignAction::Walk, {-793, -3088, 0}}, {ECampaignAction::Use, {-793, -3136, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 143}, {ECampaignAction::Walk, {-793, -3545, 0}},
        {ECampaignAction::Use, {-793, -3585, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 164},
        {ECampaignAction::Walk, {-771, -3631, 0}}, {ECampaignAction::Walk, {-486, -3743, 0}},
        {ECampaignAction::Walk, {486, -3642, 0}}, {ECampaignAction::Walk, {538, -3600, 0}},
        {ECampaignAction::Use, {546, -3567, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 181},
        {ECampaignAction::Walk, {482, -3471, 0}}, {ECampaignAction::Walk, {450, -3160, 0}},
        {ECampaignAction::Walk, {480, -3110, 0}}, {ECampaignAction::Walk, {611, -3088, 0}},
        {ECampaignAction::Use, {656, -3088, 40}},
        {ECampaignAction::Floor, {0, 0, 32}, 33, TEXT("L34's original switch lowers the lighthouse exit floor")},
        {ECampaignAction::Walk, {480, -3110, 0}}, {ECampaignAction::Walk, {450, -3160, 0}},
        {ECampaignAction::Walk, {482, -3471, 0}}, {ECampaignAction::Walk, {540, -3532, 0}},
        {ECampaignAction::Use, {546, -3564, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 181},
        {ECampaignAction::Walk, {538, -3611, 0}}, {ECampaignAction::Walk, {-486, -3743, 0}},
        {ECampaignAction::Walk, {-793, -3645, 0}}, {ECampaignAction::Use, {-793, -3601, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 164}, {ECampaignAction::Walk, {-793, -3197, 0}},
        {ECampaignAction::Use, {-793, -3152, 44}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 143},
        {ECampaignAction::Walk, {-795, -2665, 0}}, {ECampaignAction::Use, {-795, -2621, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 163}, {ECampaignAction::Walk, {-795, -2406, 0}},
        {ECampaignAction::Walk, {-795, -2346, 0}}, {ECampaignAction::Walk, {-795, -2406, 0}},
        {ECampaignAction::CeilingAtMost, {0, 0, 90}, 160}, {ECampaignAction::OpenDoor, {0, 0, 64}, 160},
        {ECampaignAction::Walk, {-795, -2011, 0}}, {ECampaignAction::Use, {-795, -1964, 44}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 134}, {ECampaignAction::Walk, {-794, -1887, 0}},
        {ECampaignAction::Walk, {-304, -1882, 0}}, {ECampaignAction::Walk, {-306, -1620, 0}},
        {ECampaignAction::Walk, {-610, -1609, 0}}, {ECampaignAction::Use, {-611, -1639, -54}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 172}, {ECampaignAction::Walk, {-611, -1708, 0}},
        {ECampaignAction::Walk, {-646, -1752, 0}}, {ECampaignAction::Walk, {-1086, -1791, 0}},
        {ECampaignAction::Walk, {-1180, -1824, 0}}, {ECampaignAction::Use, {-1220, -1824, -54}},
        {ECampaignAction::Floor, {0, 0, -96}, 175, TEXT("L34's hidden switch opens the return passage")},
        {ECampaignAction::Walk, {-878, -1765, 0}}, {ECampaignAction::Walk, {-641, -1754, 0}},
        {ECampaignAction::Walk, {-611, -1700, 0}}, {ECampaignAction::Use, {-611, -1647, -54}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 172}, {ECampaignAction::Walk, {-611, -1444, 0}},
        {ECampaignAction::Use, {-611, -1408, -54}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 176},
        {ECampaignAction::Walk, {-593, -1330, 0}}, {ECampaignAction::Walk, {-545, -1296, 0}},
        {ECampaignAction::Walk, {-542, -1271, 0}}, {ECampaignAction::Walk, {-521, -1222, 0}},
        {ECampaignAction::Checkpoint, {}, INDEX_NONE, LighthouseReturnSlot},
        {ECampaignAction::Walk, {-558, -1111, 0}}, {ECampaignAction::Walk, {-593, -1106, 0}},
        {ECampaignAction::Pickup, {-612, -1098, -88}, HealingPotion, TEXT("an authored lighthouse healing potion")},
        {ECampaignAction::Walk, {-579, -1075, 0}}, {ECampaignAction::Pickup, {-608, -1058, -88}, HealingPotion},
        {ECampaignAction::Walk, {-493, -1100, 0}}, {ECampaignAction::Pickup, {-468, -1096, -88}, HealingPotion},
        {ECampaignAction::Pickup, {-480, -1064, -88}, HealingPotion},
        {ECampaignAction::Walk, {-527, -1216, 0}},
        {ECampaignAction::Teleport, {-441, -1216, 0}, INDEX_NONE, TEXT("L34's return teleporter reaches the lighthouse"), {-1038, 144}},
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3400}},
        {ECampaignAction::Walk, {-1006, -263, 0}}, {ECampaignAction::Walk, {-791, -271, 0}},
        {ECampaignAction::Walk, {-750, -177, 0}}, {ECampaignAction::Walk, {-702, 753, 0}},
        {ECampaignAction::Walk, {-480, 978, 0}}, {ECampaignAction::Walk, {-381, 959, 0}},
        {ECampaignAction::Walk, {-381, 918, 0}},
        {ECampaignAction::DefeatCreature, {}, 213, TEXT("the skeleton guarding L34's lowered gate")},
        {ECampaignAction::DefeatCreature, {}, 215, TEXT("the skeleton beyond L34's glowers")},
        {ECampaignAction::Jump, {-381, 841, 0}, 82},
        {ECampaignAction::Walk, {-351, 830, 0}},
        {ECampaignAction::Jump, {-240, 830, 0}, 113},
        {ECampaignAction::Walk, {-135, 830, 0}}, {ECampaignAction::Walk, {-29, 868, 0}},
    };
    const FCampaignAction Campaign151[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2000}}, {ECampaignAction::ManaAtLeast, {0, 0, 2000}},
        {ECampaignAction::Walk, {531, 3418, 0}}, {ECampaignAction::Walk, {563, 3362, 0}},
        {ECampaignAction::Walk, {787, 2882, 0}}, {ECampaignAction::Walk, {1675, 2778, 0}},
        {ECampaignAction::Walk, {2083, 2538, 0}}, {ECampaignAction::Walk, {2307, 2378, 0}},
        {ECampaignAction::Walk, {2363, 2330, 0}}, {ECampaignAction::Walk, {2819, 1842, 0}},
        {ECampaignAction::Walk, {3403, 1858, 0}}, {ECampaignAction::Walk, {3507, 1882, 0}},
        {ECampaignAction::Walk, {3595, 1986, 0}}, {ECampaignAction::Walk, {3539, 2298, 0}},
        {ECampaignAction::Walk, {2819, 3018, 0}}, {ECampaignAction::Walk, {2624, 3051, 0}},
        {ECampaignAction::Pickup, {2586, 3051, -52}, NativeWadRules::ElmoreKey5, TEXT("the L151 cave key")},
        {ECampaignAction::Walk, {2915, 3186, 0}}, {ECampaignAction::Walk, {2979, 4066, 0}},
        {ECampaignAction::Walk, {2667, 4162, 0}}, {ECampaignAction::Walk, {2624, 4204, 0}},
        {ECampaignAction::Use, {2624, 4256, 9}}, {ECampaignAction::OpenDoor, {0, 0, 58}, 48},
        {ECampaignAction::Walk, {2624, 4336, 0}},
    };
    const FCampaignAction Campaign152[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 1950}}, {ECampaignAction::ManaAtLeast, {0, 0, 3200}},
        {ECampaignAction::Walk, {1446, 2066, 0}}, {ECampaignAction::Use, {1446, 2019, 121}},
        {ECampaignAction::OpenDoor, {0, 0, 64}, 50}, {ECampaignAction::Walk, {1446, 1992, 0}},
        {ECampaignAction::Walk, {1470, 1816, 0}}, {ECampaignAction::Walk, {1742, 1488, 0}},
        {ECampaignAction::Walk, {1734, 1320, 0}}, {ECampaignAction::Walk, {1638, 1032, 0}},
        {ECampaignAction::Walk, {1630, 1024, 0}}, {ECampaignAction::Walk, {822, 608, 0}},
        {ECampaignAction::Walk, {742, 544, 0}}, {ECampaignAction::Walk, {-274, 544, 0}},
        {ECampaignAction::Walk, {-602, 504, 0}}, {ECampaignAction::Walk, {-658, 448, 0}},
        {ECampaignAction::Walk, {-634, 312, 0}}, {ECampaignAction::Walk, {-390, 66, 0}},
        {ECampaignAction::Jump, {-390, -45, 0}, 106}, {ECampaignAction::Walk, {-421, -107, 0}},
        {ECampaignAction::Pickup, {-388, -130, 140}, NativeWadRules::ElmoreKey6, TEXT("the L152 reservoir key")},
        {ECampaignAction::Walk, {-568, -152, 0}}, {ECampaignAction::Use, {-531.5, -134, 77}},
        {ECampaignAction::Floor, {0, 0, -476}, 27, TEXT("L152's hidden switch lowers the reservoir floor")},
        {ECampaignAction::Walk, {-308, -321, 0}}, {ECampaignAction::Walk, {-306, -460, 0}},
        {ECampaignAction::Pickup, {-324, -487, -464}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Pickup, {-296, -486, -464}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Pickup, {-269, -486, -464}, NativeWadRules::FlyingPotion},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {-306, -322, -407}}, {ECampaignAction::Fly, {-302, -258, -395}},
        {ECampaignAction::Fly, {-302, -258, 170}}, {ECampaignAction::Fly, {-565, -135, 170}},
        {ECampaignAction::FlightEnded, {}, INDEX_NONE, TEXT("L152's flight supplies leave two carried potions")},
        {ECampaignAction::Walk, {-658, 448, 0}}, {ECampaignAction::Jump, {-490, 536, 0}, 180},
        {ECampaignAction::Walk, {-378, 488, 0}}, {ECampaignAction::Walk, {-42, 49, 0}},
        {ECampaignAction::Use, {-48, -3, 40}}, {ECampaignAction::OpenDoor, {0, 0, 64}, 54},
        {ECampaignAction::Walk, {-25, -94, 0}}, {ECampaignAction::Walk, {-16, -250, 0}},
    };
    const FCampaignAction Campaign153[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2050}}, {ECampaignAction::ManaAtLeast, {0, 0, 3200}},
        {ECampaignAction::Walk, {1552, 2433, 0}}, {ECampaignAction::Use, {1552, 2395, 64}},
        {ECampaignAction::OpenDoor, {0, 0, 75}, 66}, {ECampaignAction::Walk, {1466, 2216, 0}},
        {ECampaignAction::Walk, {531, 1895, 0}}, {ECampaignAction::Walk, {471, 1891, 0}},
        {ECampaignAction::Jump, {471, 1799, 0}, 105},
        {ECampaignAction::Walk, {130, 1848, 0}},
        {ECampaignAction::Walk, {-886, 1560, 0}}, {ECampaignAction::Walk, {-1526, 1200, 0}},
        {ECampaignAction::Walk, {-1638, 1072, 0}}, {ECampaignAction::Walk, {-1570, 675, 0}},
        {ECampaignAction::Jump, {-1558, 585, 0}, 90},
        {ECampaignAction::Walk, {-918, 120, 0}}, {ECampaignAction::Walk, {-942, -273, 0}},
        {ECampaignAction::Pickup, {-940, -306, -68}, NativeWadRules::ElmoreKey8, TEXT("the L153 eastern vault key")},
        {ECampaignAction::Walk, {-918, 120, 0}}, {ECampaignAction::Walk, {-998, 160, 0}},
        {ECampaignAction::Jump, {-1110, 120, 0}, 115}, {ECampaignAction::Walk, {-1126, -360, 0}},
        {ECampaignAction::Walk, {-542, -568, 0}}, {ECampaignAction::Walk, {-406, -568, 0}},
        {ECampaignAction::Walk, {-206, -536, 0}}, {ECampaignAction::Walk, {330, -320, 0}},
        {ECampaignAction::Walk, {378, -280, 0}}, {ECampaignAction::Walk, {818, 112, 0}},
        {ECampaignAction::Walk, {913, 179, 0}}, {ECampaignAction::Use, {940, 191, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 212}, {ECampaignAction::Walk, {1018, 246, 0}},
        {ECampaignAction::Walk, {1088, 254, 0}}, {ECampaignAction::Use, {1088, 224, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 96}, {ECampaignAction::Walk, {1088, -202, 0}},
        {ECampaignAction::Walk, {1026, -272, 0}}, {ECampaignAction::Walk, {1010, -392, 0}},
        {ECampaignAction::Walk, {890, -423, 0}}, {ECampaignAction::Use, {864, -448, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 107}, {ECampaignAction::Walk, {864, -528, 0}},
        {ECampaignAction::Use, {864, -560, 64}},
        {ECampaignAction::Ceiling, {0, 0, 108}, 102, TEXT("L153's first switch opens the passage to the key doors")},
        {ECampaignAction::Walk, {864, -490, 0}}, {ECampaignAction::Use, {864, -464, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 107}, {ECampaignAction::Walk, {906, -416, 0}},
        {ECampaignAction::Walk, {1002, -400, 0}}, {ECampaignAction::Walk, {1042, -240, 0}},
        {ECampaignAction::Walk, {1088, -176, 0}}, {ECampaignAction::Walk, {1088, 173, 0}},
        {ECampaignAction::Use, {1088, 208, 72}}, {ECampaignAction::OpenDoor, {0, 0, 70}, 96},
        {ECampaignAction::Walk, {1108, 320, 0}}, {ECampaignAction::Use, {1152, 320, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 92}, {ECampaignAction::Walk, {1476, 320, 0}},
        {ECampaignAction::Walk, {1504, 299, 0}}, {ECampaignAction::Walk, {1504, 72, 0}},
        {ECampaignAction::Use, {1504, 32, 15}}, {ECampaignAction::OpenDoor, {0, 0, 70}, 112},
        {ECampaignAction::Walk, {1490, -136, 0}}, {ECampaignAction::Walk, {1410, -216, 0}},
        // The west plate closes the gate behind the player. The bank releases it for the return.
        {ECampaignAction::Walk, {1426, -360, 0}}, {ECampaignAction::Walk, {1450, -384, 0}},
        {ECampaignAction::Walk, {1618, -448, 0}}, {ECampaignAction::Walk, {1682, -504, 0}},
        {ECampaignAction::Walk, {1682, -552, 0}}, {ECampaignAction::Walk, {1738, -592, 0}},
        {ECampaignAction::Walk, {1794, -664, 0}}, {ECampaignAction::Walk, {1850, -712, 0}},
        {ECampaignAction::Walk, {1823, -854, 0}}, {ECampaignAction::Ceiling, {0, 0, -16}, 218},
        {ECampaignAction::Walk, {1826, -1152, 0}}, {ECampaignAction::Walk, {1890, -1232, 0}},
        {ECampaignAction::Walk, {1842, -1760, 0}}, {ECampaignAction::Walk, {1818, -1792, 0}},
        {ECampaignAction::Walk, {1754, -1808, 0}}, {ECampaignAction::Walk, {1746, -1864, 0}},
        {ECampaignAction::Walk, {1778, -1992, 0}}, {ECampaignAction::Walk, {1840, -2035, 0}},
        {ECampaignAction::Use, {1840, -2064, 0}},
        {ECampaignAction::Ceiling, {0, 0, 200}, 218, TEXT("L153's west switch combination reopens the gate")},
        {ECampaignAction::Walk, {1778, -1992, 0}}, {ECampaignAction::Walk, {1746, -1840, 0}},
        {ECampaignAction::Walk, {1770, -1792, 0}}, {ECampaignAction::Walk, {1842, -1768, 0}},
        {ECampaignAction::Walk, {1890, -1272, 0}}, {ECampaignAction::Walk, {1826, -1136, 0}},
        {ECampaignAction::Walk, {1842, -696, 0}}, {ECampaignAction::Walk, {1786, -656, 0}},
        {ECampaignAction::Walk, {1690, -560, 0}}, {ECampaignAction::Walk, {1682, -504, 0}},
        {ECampaignAction::Walk, {1642, -464, 0}}, {ECampaignAction::Walk, {1434, -368, 0}},
        {ECampaignAction::Walk, {1410, -328, 0}}, {ECampaignAction::Walk, {1394, -166, 0}},
        {ECampaignAction::Walk, {1250, -156, 0}}, {ECampaignAction::Use, {1250, -192, 14}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 118}, {ECampaignAction::Walk, {1234, -520, 0}},
        {ECampaignAction::Walk, {1210, -544, 0}}, {ECampaignAction::Walk, {1050, -560, 0}},
        {ECampaignAction::Walk, {1034, -584, 0}}, {ECampaignAction::Walk, {1042, -648, 0}},
        {ECampaignAction::Walk, {1106, -696, 0}}, {ECampaignAction::Walk, {1106, -744, 0}},
        {ECampaignAction::Walk, {1018, -784, 0}}, {ECampaignAction::Walk, {978, -936, 0}},
        {ECampaignAction::Walk, {954, -960, 0}}, {ECampaignAction::Walk, {858, -976, 0}},
        {ECampaignAction::Walk, {832, -995, 0}}, {ECampaignAction::Use, {832, -1033, 155}},
        {ECampaignAction::OpenDoor, {0, 0, 64}, 162}, {ECampaignAction::Walk, {810, -1128, 0}},
        {ECampaignAction::Walk, {486, -1452, 0}},
        {ECampaignAction::Pickup, {460, -1480, 124}, NativeWadRules::ElmoreKey3, TEXT("the L153 center vault key")},
        {ECampaignAction::Walk, {834, -1082, 0}}, {ECampaignAction::Use, {832, -1049, 155}},
        {ECampaignAction::OpenDoor, {0, 0, 64}, 162}, {ECampaignAction::Walk, {850, -984, 0}},
        {ECampaignAction::Walk, {874, -960, 0}}, {ECampaignAction::Walk, {970, -944, 0}},
        {ECampaignAction::Walk, {1010, -792, 0}}, {ECampaignAction::Walk, {1106, -744, 0}},
        {ECampaignAction::Walk, {1106, -696, 0}}, {ECampaignAction::Walk, {1050, -656, 0}},
        {ECampaignAction::Walk, {1034, -632, 0}}, {ECampaignAction::Walk, {1042, -568, 0}},
        {ECampaignAction::Walk, {1226, -528, 0}}, {ECampaignAction::Walk, {1250, -488, 0}},
        {ECampaignAction::Walk, {1250, -241, 0}}, {ECampaignAction::Use, {1250, -208, 12}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 118}, {ECampaignAction::Walk, {1290, -160, 0}},
        {ECampaignAction::Walk, {1522, -144, 0}}, {ECampaignAction::Walk, {1600, -157, 0}},
        {ECampaignAction::Use, {1600, -128, 18}}, {ECampaignAction::OpenDoor, {0, 0, 64}, 116},
        {ECampaignAction::Walk, {1618, -56, 0}}, {ECampaignAction::Walk, {1642, -32, 0}},
        {ECampaignAction::Walk, {1770, -48, 0}}, {ECampaignAction::Walk, {1778, -136, 0}},
        {ECampaignAction::Walk, {1802, -160, 0}}, {ECampaignAction::Walk, {1944, -160, 0}},
        {ECampaignAction::Use, {1984, -160, -107}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 130},
        {ECampaignAction::Walk, {2498, -144, 0}}, {ECampaignAction::Walk, {2562, -80, 0}},
        {ECampaignAction::Walk, {2626, -48, 0}}, {ECampaignAction::Walk, {2674, -64, 0}},
        {ECampaignAction::Walk, {2747, -160, 0}}, {ECampaignAction::Use, {2784, -160, -105}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 137}, {ECampaignAction::Walk, {3083, -160, 0}},
        {ECampaignAction::Use, {3120, -160, -105}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 174},
        {ECampaignAction::Walk, {3180, -160, 0}},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {3552, -160, -37}},
        {ECampaignAction::Fly, {3704, -160, -50}, INDEX_NONE, TEXT("L153's flight potion clears the Gargoyle corridor")},
        {ECampaignAction::Walk, {3722, -160, 0}},
        {ECampaignAction::Use, {3760, -160, -105}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 193},
        {ECampaignAction::Walk, {4330, -176, 0}}, {ECampaignAction::Walk, {4394, -240, 0}},
        {ECampaignAction::Use, {4432, -240, -112}},
        {ECampaignAction::Floor, {0, 0, -148}, 202, TEXT("L153's orb switch combination opens the vault")},
        {ECampaignAction::Walk, {4394, -160, 0}}, {ECampaignAction::Walk, {4485, -160, 0}},
        {ECampaignAction::Walk, {4590, -195, 0}}, {ECampaignAction::Walk, {4612, -291, 0}},
        {ECampaignAction::Pickup, {4612, -330, -136}, NativeWadRules::OrbOfDefense, TEXT("the original Orb of Defense")},
        {ECampaignAction::Walk, {4594, -208, 0}}, {ECampaignAction::Walk, {4490, -160, 0}},
        {ECampaignAction::Use, {4448, -160, -106}}, {ECampaignAction::Floor, {0, 0, -148}, 202},
        {ECampaignAction::Fly, {3810, -160, -50}}, {ECampaignAction::Use, {3768, -160, -105}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 193}, {ECampaignAction::Fly, {3474, -160, -50}},
        {ECampaignAction::Fly, {3180, -160, -30}, INDEX_NONE, TEXT("L153's flight potion clears the return corridor")},
        {ECampaignAction::UseLockedDoor, {3144, -160, -75}, 174}, {ECampaignAction::Fly, {3170, -160, -30}},
        {ECampaignAction::Fly, {3088, -160, -30}}, {ECampaignAction::Fly, {2835, -160, -44}},
        {ECampaignAction::UseLockedDoor, {2800, -160, -75}, 137},
        {ECampaignAction::Fly, {2690, -80, -44}},
        {ECampaignAction::FlightEnded, {}, INDEX_NONE, TEXT("L153's flight ends before climbing the return stairs")},
        {ECampaignAction::Walk, {2626, -48, 0}},
        {ECampaignAction::Walk, {2578, -64, 0}}, {ECampaignAction::Walk, {2458, -160, 0}},
        {ECampaignAction::Walk, {2040, -160, 0}}, {ECampaignAction::Use, {2000, -160, -105}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 130}, {ECampaignAction::Walk, {1786, -144, 0}},
        {ECampaignAction::Walk, {1778, -56, 0}}, {ECampaignAction::Walk, {1754, -32, 0}},
        {ECampaignAction::Walk, {1614, -57, 0}}, {ECampaignAction::Walk, {1600, -80, 0}},
        {ECampaignAction::Use, {1600, -112, 16}}, {ECampaignAction::OpenDoor, {0, 0, 64}, 116},
        {ECampaignAction::Walk, {1530, -144, 0}}, {ECampaignAction::Walk, {1506, -104, 0}},
        {ECampaignAction::Walk, {1504, -19, 0}}, {ECampaignAction::Use, {1504, 16, 15}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 112}, {ECampaignAction::Walk, {1504, 309, 0}},
        {ECampaignAction::Walk, {1206, 320, 0}}, {ECampaignAction::Use, {1168, 320, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 92}, {ECampaignAction::Walk, {1020, 246, 0}},
        {ECampaignAction::Walk, {976, 232, 0}}, {ECampaignAction::Use, {950, 207, 72}},
        {ECampaignAction::OpenDoor, {0, 0, 70}, 212}, {ECampaignAction::Walk, {867, 152, 0}},
        {ECampaignAction::Walk, {834, -32, 0}}, {ECampaignAction::Walk, {834, -208, 0}},
        {ECampaignAction::Walk, {832, -282, 0}},
    };
    const FCampaignAction Campaign231[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2700}}, {ECampaignAction::ManaAtLeast, {0, 0, 2000}},
        {ECampaignAction::Walk, {-2216, 3432, 0}}, {ECampaignAction::Walk, {-2216, 3480, 0}},
        {ECampaignAction::Walk, {-1200, 4496, 0}}, {ECampaignAction::Walk, {-1136, 4680, 0}},
        {ECampaignAction::Walk, {-536, 4680, 0}}, {ECampaignAction::Walk, {-128, 4152, 0}},
        {ECampaignAction::Walk, {-112, 3136, 0}}, {ECampaignAction::Walk, {-112, 2924, 0}},
        {ECampaignAction::Use, {-112, 2879, 252}}, {ECampaignAction::OpenDoor, {0, 0, 64}, 255},
        {ECampaignAction::Walk, {-56, 2776, 0}}, {ECampaignAction::Walk, {381, 2236, 0}},
        {ECampaignAction::Use, {423, 2236, 250}}, {ECampaignAction::OpenDoor, {0, 0, 80}, 271},
        {ECampaignAction::Walk, {448, 2224, 0}}, {ECampaignAction::Walk, {888, 2176, 0}},
        {ECampaignAction::Walk, {935, 2048, 0}}, {ECampaignAction::Use, {936, 2007, 186}},
        {ECampaignAction::OpenDoor, {0, 0, 72}, 17}, {ECampaignAction::Walk, {936, 1742, 0}},
        {ECampaignAction::Use, {936, 1698, 186}}, {ECampaignAction::OpenDoor, {0, 0, 64}, 16},
        {ECampaignAction::Walk, {936, 1480, 0}}, {ECampaignAction::Walk, {664, 1376, 0}},
        {ECampaignAction::Walk, {648, 360, 0}}, {ECampaignAction::Walk, {760, 288, 0}},
        {ECampaignAction::Walk, {936, 232, 0}}, {ECampaignAction::Walk, {928, -96, 0}},
        {ECampaignAction::Walk, {862, -157, 0}}, {ECampaignAction::Use, {862, -194, 188}},
        {ECampaignAction::OpenDoor, {0, 0, 64}, 268}, {ECampaignAction::Walk, {864, -256, 0}},
        {ECampaignAction::Walk, {864, -320, 0}},
    };
    const FCampaignAction Campaign75[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2700}}, {ECampaignAction::ManaAtLeast, {0, 0, 2200}},
        {ECampaignAction::Walk, {2099, 2742, 0}}, {ECampaignAction::Use, {2099, 2700, 30}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 156}, {ECampaignAction::Walk, {2099, 2526, 0}},
        {ECampaignAction::Walk, {2104, 1956, 0}}, {ECampaignAction::Use, {2104, 1914, 30}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 99}, {ECampaignAction::Walk, {2104, 1856, 0}},
        {ECampaignAction::Walk, {2323, 1822, 0}}, {ECampaignAction::Walk, {2511, 1510, 0}},
        {ECampaignAction::Walk, {2435, 1186, 0}}, {ECampaignAction::Walk, {2271, 1054, 0}},
        {ECampaignAction::Walk, {1999, 1082, 0}}, {ECampaignAction::Walk, {1900, 1180, 0}},
        {ECampaignAction::TouchSector, {1900, 1240, 0}, 35}, {ECampaignAction::Walk, {1900, 1180, 0}},
        {ECampaignAction::Floor, {0, 0, -16}, 192, TEXT("L75's pit trigger lowers the passage platform")},
        {ECampaignAction::UseFlightItem, {}, NativeWadRules::FlyingPotion},
        {ECampaignAction::Fly, {1764, 1328, 50}},
        {ECampaignAction::Fly, {1764, 1328, -1738}},
        {ECampaignAction::Pickup, {1742, 1303, 0}, NativeWadRules::FlyingPotion, TEXT("L75's authored flight potion in the pit")},
        {ECampaignAction::Fly, {1744, 1274, -1738}},
        {ECampaignAction::Pickup, {1724, 1283, 0}, NativeWadRules::FlyingPotion, TEXT("L75's second authored pit flight potion")},
        {ECampaignAction::Fly, {1900, 1180, 50}},
        {ECampaignAction::FlightEnded, {}, INDEX_NONE, TEXT("L75's two collected flight potions are carried out of the pit")},
        {ECampaignAction::Walk, {1999, 1082, 0}}, {ECampaignAction::Walk, {2271, 1054, 0}},
        {ECampaignAction::Walk, {2435, 1186, 0}}, {ECampaignAction::Walk, {2511, 1510, 0}},
        {ECampaignAction::Walk, {2323, 1822, 0}}, {ECampaignAction::Walk, {2059, 1859, 0}},
        {ECampaignAction::Walk, {1819, 1794, 0}}, {ECampaignAction::Walk, {1646, 1789, 0}},
        {ECampaignAction::Walk, {1635, 1806, 0}}, {ECampaignAction::Walk, {1547, 1904, 0}},
        {ECampaignAction::Walk, {1511, 1921, 0}}, {ECampaignAction::Use, {1472, 1921, -22}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 106}, {ECampaignAction::Walk, {1447, 1910, 0}},
        {ECampaignAction::Walk, {1207, 1538, 0}}, {ECampaignAction::Walk, {1235, 1398, 0}},
        {ECampaignAction::Walk, {1523, 1210, 0}}, {ECampaignAction::Walk, {1507, 990, 0}},
        {ECampaignAction::Walk, {1479, 962, 0}}, {ECampaignAction::Walk, {1323, 982, 0}},
        {ECampaignAction::Walk, {1291, 1014, 0}}, {ECampaignAction::Walk, {1267, 1178, 0}},
        {ECampaignAction::Walk, {1079, 1186, 0}}, {ECampaignAction::Walk, {1001, 1039, 0}},
        {ECampaignAction::Walk, {981, 766, 0}}, {ECampaignAction::Use, {981, 729, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 13}, {ECampaignAction::Walk, {923, 678, 0}},
        {ECampaignAction::Walk, {415, 302, 0}}, {ECampaignAction::Walk, {100, 284, 0}},
        {ECampaignAction::Use, {60, 284, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 5},
        {ECampaignAction::Walk, {-93, 322, 0}}, {ECampaignAction::Walk, {-248, 413, 0}},
        {ECampaignAction::Use, {-248, 449, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 3},
        {ECampaignAction::Walk, {-249, 494, 0}}, {ECampaignAction::Walk, {-219, 680, 0}},
        {ECampaignAction::Walk, {-220, 751, 0}}, {ECampaignAction::Walk, {-379, 751, 0}},
        {ECampaignAction::Walk, {-389, 786, 0}},
        {ECampaignAction::Use, {-429, 786, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 45},
        {ECampaignAction::Walk, {-480, 786, 0}}, {ECampaignAction::Walk, {-481, 862, 0}},
        {ECampaignAction::Walk, {-465, 970, 0}}, {ECampaignAction::Walk, {-257, 1022, 0}},
        {ECampaignAction::Walk, {1, 1023, 0}}, {ECampaignAction::Use, {29, 1023, 44}},
        {ECampaignAction::Floor, {0, 0, 16}, 48, TEXT("L75's exit switch lowers the first lift")},
        {ECampaignAction::Walk, {-469, 974, 0}}, {ECampaignAction::Walk, {-749, 167, 0}},
        {ECampaignAction::Use, {-749, 129, 129}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 58},
        {ECampaignAction::Walk, {-837, -67, 0}}, {ECampaignAction::Use, {-841, -110, 131}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 103}, {ECampaignAction::Walk, {-845, -153, 0}},
        {ECampaignAction::Walk, {-888, -210, 0}}, {ECampaignAction::Use, {-928, -210, 131}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 100}, {ECampaignAction::Walk, {-1050, -210, 0}},
    };
    const FCampaignAction Campaign10[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2700}}, {ECampaignAction::ManaAtLeast, {0, 0, 2200}},
        {ECampaignAction::Walk, {-1210, -674, 0}}, {ECampaignAction::Use, {-1210, -708, 210}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 192}, {ECampaignAction::Walk, {-1165, -734, 0}},
        {ECampaignAction::Walk, {-1117, -734, 0}}, {ECampaignAction::Walk, {-957, -526, 0}},
        {ECampaignAction::Walk, {-957, -442, 0}}, {ECampaignAction::Walk, {-931, -370, 0}},
        {ECampaignAction::Walk, {-864, -326, 0}}, {ECampaignAction::Walk, {-709, -174, 0}},
        {ECampaignAction::Walk, {-499, -48, 0}},
        {ECampaignAction::Use, {-471, -48, 164}},
        {ECampaignAction::Floor, {0, 0, 100}, 120, TEXT("L10's first switch lowers the key platform to Z100")},
        {ECampaignAction::Walk, {-837, -254, 0}}, {ECampaignAction::Walk, {-1053, -238, 0}},
        {ECampaignAction::Walk, {-1061, -230, 0}}, {ECampaignAction::Walk, {-1274, 309, 0}},
        {ECampaignAction::Use, {-1312, 309, 160}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 70},
        {ECampaignAction::Walk, {-1845, 370, 0}}, {ECampaignAction::Walk, {-1989, 714, 0}},
        {ECampaignAction::Walk, {-1925, 1130, 0}}, {ECampaignAction::Walk, {-2103, 1408, 0}},
        {ECampaignAction::Use, {-2139, 1440, 142}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 211},
        {ECampaignAction::Walk, {-2397, 1698, 0}}, {ECampaignAction::Walk, {-2417, 1696, 0}},
        {ECampaignAction::Use, {-2445, 1696, 44}},
        {ECampaignAction::Floor, {0, 0, 0}, 120, TEXT("L10's second switch lowers the key platform to Z0")},
        {ECampaignAction::Walk, {-2116, 1698, 0}}, {ECampaignAction::Use, {-2076, 1698, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 65}, 27}, {ECampaignAction::Walk, {-1694, 1698, 0}},
        {ECampaignAction::Use, {-1640, 1698, 43}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 206},
        {ECampaignAction::Walk, {-1397, 1698, 0}}, {ECampaignAction::Walk, {-983, 1793, 0}},
        {ECampaignAction::Pickup, {-1001, 1802, 0}, GreaterHealingPotion, TEXT("L10's western platform healing potion")},
        {ECampaignAction::Pickup, {-956, 1771, 12}, NativeWadRules::ElmoreKey6, TEXT("the original L10 platform key")},
        {ECampaignAction::Walk, {-932, 1753, 0}},
        {ECampaignAction::Pickup, {-914, 1739, 0}, GreaterHealingPotion, TEXT("L10's eastern platform healing potion")},
        {ECampaignAction::Walk, {-957, 2274, 0}}, {ECampaignAction::Walk, {-1189, 2586, 0}},
        {ECampaignAction::Walk, {-1157, 2698, 0}}, {ECampaignAction::Walk, {-969, 2905, 0}},
        {ECampaignAction::Use, {-969, 2932, 128}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 200},
        {ECampaignAction::Walk, {-949, 3092, 0}}, {ECampaignAction::Walk, {-917, 3138, 0}},
        {ECampaignAction::Walk, {-720, 3168, 0}},
    };
    const FCampaignAction Campaign12[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 3200}}, {ECampaignAction::ManaAtLeast, {0, 0, 4300}},
        {ECampaignAction::Walk, {3510, 1168, 0}}, {ECampaignAction::Walk, {2630, 1472, 0}},
        {ECampaignAction::Walk, {2526, 1816, 0}}, {ECampaignAction::Walk, {2606, 2320, 0}},
        {ECampaignAction::Walk, {3334, 2432, 0}}, {ECampaignAction::Walk, {3734, 3104, 0}},
        {ECampaignAction::Use, {3780, 3104, 43}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 172},
        {ECampaignAction::Walk, {4294, 3120, 0}}, {ECampaignAction::Walk, {4318, 3168, 0}},
        {ECampaignAction::Walk, {4302, 3592, 0}}, {ECampaignAction::Walk, {4266, 3616, 0}},
        {ECampaignAction::Use, {4224, 3616, 43}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 229},
        {ECampaignAction::Walk, {4142, 3600, 0}}, {ECampaignAction::Walk, {4040, 3340, 0}},
        {ECampaignAction::Pickup, {4040, 3376, 12}, NativeWadRules::Key1, TEXT("the original L12 key")},
        {ECampaignAction::Walk, {4150, 3616, 0}}, {ECampaignAction::Use, {4192, 3616, 43}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 229}, {ECampaignAction::Walk, {4294, 3600, 0}},
        {ECampaignAction::Walk, {4318, 3552, 0}}, {ECampaignAction::Walk, {4302, 3128, 0}},
        {ECampaignAction::Walk, {4254, 3104, 0}}, {ECampaignAction::Walk, {3838, 3104, 0}},
        {ECampaignAction::Use, {3796, 3104, 43}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 172},
        {ECampaignAction::Walk, {3502, 3128, 0}},
        {ECampaignAction::Walk, {2486, 3176, 0}}, {ECampaignAction::Walk, {1568, 3320, 0}},
        {ECampaignAction::Use, {1568, 3360, 63}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 170},
        {ECampaignAction::Walk, {1568, 3420, 0}}, {ECampaignAction::Walk, {1512, 3540, 0}},
        {ECampaignAction::Walk, {1436, 3640, 0}},
        {ECampaignAction::Pickup, {1424, 3662, 0}, GreaterHealingPotion, TEXT("L12's healing potion before the key door")},
        {ECampaignAction::Walk, {1352, 3553, 0}}, {ECampaignAction::Walk, {1320, 3616, 0}},
        {ECampaignAction::Use, {1276, 3616, 63}}, {ECampaignAction::OpenDoor, {0, 0, 65}, 81},
        {ECampaignAction::Walk, {1088, 3613, 0}}, {ECampaignAction::TouchSector, {1088, 3551, 0}, 52},
        {ECampaignAction::Walk, {1088, 3616, 0}},
        {ECampaignAction::Floor, {0, 0, 30}, 56, TEXT("L12's timed plate raises the passage floor")},
        {ECampaignAction::Walk, {650, 3616, 0}}, {ECampaignAction::Walk, {40, 3616, 0}},
        {ECampaignAction::Use, {0, 3616, 63}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 287},
        {ECampaignAction::Walk, {-248, 3616, 0}}, {ECampaignAction::Walk, {-290, 3552, 0}},
        {ECampaignAction::Walk, {-266, 3272, 0}}, {ECampaignAction::Walk, {-162, 3120, 0}},
        {ECampaignAction::Walk, {-186, 3040, 0}}, {ECampaignAction::Walk, {-449, 2944, 0}},
        {ECampaignAction::Floor, {0, 0, -120}, 136, TEXT("L12's lower corridor opens the descent")},
        {ECampaignAction::Walk, {-1290, 2992, 0}}, {ECampaignAction::Walk, {-1306, 3024, 0}},
        {ECampaignAction::Walk, {-1298, 3080, 0}}, {ECampaignAction::Walk, {-1250, 3104, 0}},
        {ECampaignAction::Walk, {-738, 3120, 0}}, {ECampaignAction::Walk, {-602, 3328, 0}},
        {ECampaignAction::Walk, {-626, 3472, 0}}, {ECampaignAction::Walk, {-914, 3616, 0}},
        {ECampaignAction::Walk, {-1344, 3600, 0}}, {ECampaignAction::Use, {-1376, 3600, -517}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 285}, {ECampaignAction::Walk, {-1436, 3600, 0}},
        {ECampaignAction::Walk, {-1704, 3600, 0}}, {ECampaignAction::Use, {-1744, 3600, -537}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 28}, {ECampaignAction::Walk, {-1784, 3600, 0}},
        {ECampaignAction::Use, {-1810, 3600, -537}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 27},
        {ECampaignAction::Walk, {-2042, 3600, 0}}, {ECampaignAction::Use, {-2082, 3600, -537}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 39}, {ECampaignAction::Walk, {-2124, 3600, 0}},
        {ECampaignAction::Use, {-2148, 3600, -537}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 33},
        {ECampaignAction::Walk, {-2380, 3600, 0}}, {ECampaignAction::Use, {-2420, 3600, -537}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 32}, {ECampaignAction::Walk, {-2462, 3600, 0}},
        {ECampaignAction::Use, {-2486, 3600, -537}}, {ECampaignAction::OpenDoor, {0, 0, 60}, 45},
        {ECampaignAction::Walk, {-2718, 3600, 0}}, {ECampaignAction::Use, {-2758, 3600, -537}},
        {ECampaignAction::OpenDoor, {0, 0, 60}, 40}, {ECampaignAction::Walk, {-2946, 3600, 0}},
        {ECampaignAction::Walk, {-3724, 3670, 0}}, {ECampaignAction::Walk, {-3806, 3671, 0}},
        {ECampaignAction::Walk, {-3895, 3671, 0}},
        {ECampaignAction::Floor, {0, 0, 0}, 294, TEXT("L12's boss chamber plate opens the Town Hall exit")},
        {ECampaignAction::Defeat, {}, NativeWadRules::OrbOfTheUndead, TEXT("Exiguus")},
        {ECampaignAction::Floor, {0, 0, -496}, 13},
        {ECampaignAction::Teleport, {-3970, 3600, 0}, 320, TEXT("L12's chamber portal returns to the entrance tunnels"), {2400, 3000}},
        {ECampaignAction::Walk, {2526, 2104, 0}}, {ECampaignAction::Walk, {2462, 1512, 0}},
        {ECampaignAction::Walk, {1880, 960, 0}},
    };
    const FCampaignRoute CampaignRoutes[] = {
        {25, 1, 20, 85, {298, 1636}, Campaign25},
        {20, 1, 60, 138, {-1084, 4630}, Campaign20},
        {60, 1, 0, 284, {-3076, -3}, Campaign60},
        {64, 2, 65, 227, {1984, 2704}, Campaign64},
        {65, 2, 34, 239, {-852, 2282}, Campaign65},
        {34, 2, 151, 180, {912, 1596}, Campaign34},
        {151, 2, 152, 84, {176, 3464}, Campaign151},
        {152, 2, 153, 266, {1484, 2315}, Campaign152},
        {153, 2, 0, 537, {1596, 2544}, Campaign153},
        {231, 3, 75, 141, {-2208, 2416}, Campaign231},
        {75, 3, 10, 140, {2096, 3034}, Campaign75},
        {10, 3, 12, 234, {-1268, -531}, Campaign10},
        {12, 3, 0, 576, {4530, 1171}, Campaign12},
        {51, 4, 52, 234, {-160, -189}, Campaign51},
        {52, 4, 41, 238, {760, 3540}, Campaign52},
        {41, 4, 44, 323, {7104, 4152}, Campaign41},
        {44, 4, 0, 71, {-3572, 758}, Campaign44},
        {74, 5, 73, 183, {-1120, -454}, Campaign74},
        {73, 5, 72, 218, {-3696, -553}, Campaign73},
        {72, 5, 0, 214, {2352, 104}, Campaign72},
        {32, 6, 71, 270, {124, 3154}, Campaign32},
        {71, 6, 70, 231, {-1146, -911}, Campaign71},
        {70, 6, 61, 58, {-2320, 492}, Campaign70},
        {61, 6, 40, 240, {2200, 2272}, Campaign61},
        {40, 6, 0, 235, {28, -4667}, Campaign40},
    };
    const FCampaignTurnIn CampaignQuests[] = {
        {1, 25, NativeWadRules::AndrewNecklace, 20, 0},
        {2, 64, NativeWadRules::OrbOfDefense, 0, 150000},
        {3, 231, NativeWadRules::OrbOfTheUndead, 50, 0},
        {4, 51, NativeWadRules::ElymasNotes, 35, 200000},
        {5, 74, NativeWadRules::WandOfDisintegration, 75, 0},
        {6, 32, NativeWadRules::OrbOfDestruction, 100, 350000},
    };
    enum class EL25Review
    {
        None, Passages, KeyReload, DoorReload, Knights, Exit, L20Reload, Scenery, L20Scenery, L20Exit, L60Reload,
        L60Puzzles, L60Relic, L60Return, L60Empty, ElmoreLaunch, ElmoreL64, ElmoreL64Reload, ElmoreL65, ElmoreL65Reload,
        ElmoreCombat64, ElmoreCombat65, ElmoreL65Exit, ElmoreL34, ElmoreL151Reload,
        ElmoreL151Exit, ElmoreL152, ElmoreL153Puzzles, ElmoreOrb, ElmoreOrbReturn, ElmoreOrbEmpty
    };
    constexpr const TCHAR* L25KeySlot = TEXT("NativeWadChurchKeyReview");
    constexpr const TCHAR* L25DoorSlot = TEXT("NativeWadChurchDoorReview");
    constexpr const TCHAR* L20ReviewSlot = TEXT("NativeWadChurchL20Review");
    constexpr const TCHAR* L60ReviewSlot = TEXT("NativeWadChurchL60Review");
    constexpr const TCHAR* AndrewPuzzleSlot = TEXT("NativeWadAndrewPuzzleReview");
    constexpr const TCHAR* AndrewRelicSlot = TEXT("NativeWadAndrewRelicReview");
    constexpr const TCHAR* AndrewTownSlot = TEXT("NativeWadAndrewTownReview");
    constexpr const TCHAR* ElmoreArrivalSlot = TEXT("NativeWadElmoreL64Review");
    constexpr const TCHAR* ElmoreSlideSlot = TEXT("NativeWadElmoreLiftReview");
    constexpr const TCHAR* ElmoreL65Slot = TEXT("NativeWadElmoreL65Review");
    constexpr const TCHAR* ElmoreTeleporterSlot = TEXT("NativeWadElmoreTeleportReview");
    constexpr const TCHAR* ElmoreL34Slot = TEXT("NativeWadElmoreL34Review");
    constexpr const TCHAR* ElmoreL151Slot = TEXT("NativeWadElmoreL151Review");
    constexpr const TCHAR* ElmoreL152Slot = TEXT("NativeWadElmoreL152Review");
    constexpr const TCHAR* ElmoreL153Slot = TEXT("NativeWadElmoreL153Review");
    constexpr const TCHAR* ElmoreVaultSlot = TEXT("NativeWadElmoreVaultReview");
    constexpr const TCHAR* ElmoreOrbSlot = TEXT("NativeWadElmoreOrbReview");
    constexpr const TCHAR* ElmoreTownSlot = TEXT("NativeWadElmoreTownReview");
    constexpr const TCHAR* UndeadCastleSlot = TEXT("NativeWadUndeadCastleReview");
    constexpr const TCHAR* UndeadTrapsSlot = TEXT("NativeWadUndeadTrapsReview");
    constexpr const TCHAR* UndeadKeepSlot = TEXT("NativeWadUndeadKeepReview");
    constexpr const TCHAR* UndeadCemeterySlot = TEXT("NativeWadUndeadCemeteryReview");
    constexpr const TCHAR* UndeadOrbSlot = TEXT("NativeWadUndeadOrbReview");
    constexpr const TCHAR* UndeadTownSlot = TEXT("NativeWadUndeadTownReview");
    constexpr const TCHAR* TempleL51Slot = TEXT("NativeWadTempleL51Review");
    constexpr const TCHAR* TempleL52Slot = TEXT("NativeWadTempleL52Review");
    constexpr const TCHAR* TempleExitSlot = TEXT("NativeWadTempleExitReview");
    constexpr const TCHAR* TownReviewSlot = TEXT("NativeWadTownReview");
    constexpr const TCHAR* FreshCampaignTownSlot = TEXT("NativeWadFreshCampaignTown0");
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
        int32 LighthouseHealInputs = 0;
        int32 CaptureFrames = 0;
        int32 CoverBolts = INDEX_NONE;
        int32 CapturedItem = INDEX_NONE;
        int32 HealthBeforeCast = INDEX_NONE;
        int32 ManaBeforeCast = INDEX_NONE;
        int32 FoodBeforeBerry = INDEX_NONE;
        int32 SliceStartingBerries = 0;
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
        bool bFreshCampaign = false;
        bool bWholeCampaign = false;
        bool bCampaignBoughtSupplies = false;
        bool bCampaignArrivalVerified = false;
        bool bArrivalCaptured = false;
        bool bCheckpointReview = false;
        bool bCheckpointVerified = false;
        bool bRiverReview = false;
        bool bRiverReload = false;
        ERiverStage RiverStage = ERiverStage::Spawn;
        FVector DriftStart = FVector::ZeroVector;
        FVector ProjectileTargetStart = FVector::ZeroVector;
        FVector2D InitialWaterUV = FVector2D::ZeroVector;
        double DriftStarted = 0.0;
        double RiverProgressTime = 0.0;
        bool bJumpStarted = false;
        bool bRavineReview = false;
        bool bAmbientReview = false;
        bool bClassicHudReview = false;
        bool bEconomyReview = false;
        bool bEconomyReload = false;
        bool bRangedReview = false;
        bool bTimedItemReload = false;
        bool bWandReview = false;
        bool bBoulderReview = false;
        bool bBoulder75Review = false;
        bool bBoulderPitTriggered = false;
        bool bElymasPresentationReview = false;
        bool bElymasPlatePrimed = false;
        TWeakObjectPtr<ANativeWadBolt> PresentationBolt;
        FVector PresentationPreviousPosition = FVector::ZeroVector;
        FVector PresentationPreviousDelta = FVector::ZeroVector;
        FVector PresentationPlayerStart = FVector::ZeroVector;
        bool bAmmoReview = false;
        bool bAmmoBannerReview = false;
        int32 DispelEffectsBefore = 0;
        bool bDispelReview = false;
        bool bDispelReload = false;
        bool bControlWandReview = false;
        bool bControlWandReload = false;
        bool bRuneProjectileReview = false;
        bool bRuneProjectileReload = false;
        int32 RuneProjectileIndex = 0;
        int32 RuneProjectileCastsBefore = 0;
        bool bConfusionWandReview = false;
        bool bConfusionWandReload = false;
        FRotator ConfusionHeading = FRotator::ZeroRotator;
        bool bResistanceReview = false;
        bool bResistanceReload = false;
        int32 ResistancePoisonBefore = 0;
        int32 ResistanceManaBefore = 0;
        bool bUtilityReview = false;
        bool bUtilityReload = false;
        bool bAmmoReload = false;
        bool bReviveReview = false;
        bool bReviveReload = false;
        bool bMusicReview = false;
        bool bCharacterReview = false;
        bool bCharacterReload = false;
        bool bClassEquipmentReview = false;
        bool bPendingCharacterReview = false;
        bool bElementalMeleeReview = false;
        bool bProjectileWandsReview = false;
        bool bRuneOwnershipReview = false;
        bool bLegacyRuneReview = false;
        bool bAuthoredGeneratorReview = false;
        bool bUndeadWizardGeneratorReview = false;
        int32 ProjectileWandIndex = 0;
        bool bInfiniteGeneratorReview = false;
        bool bLavaTerrainReview = false;
        TWeakObjectPtr<ANativeWadGuardian> ElementalTarget;
        TOptional<FVector2D> CampaignObstacleDetour;
        TOptional<FVector2D> CampaignObstacleAfter;
        TWeakObjectPtr<ANativeWadGuardian> WalkBlockingEnemy;
        bool bCampaignConcealmentVerified = false;
        bool bFightingForeground = false;
        int32 CombatStrafe = 0;
        bool bFireballWandReview = false;
        FVector ReviveEntrance = FVector::ZeroVector;
        TWeakObjectPtr<ANativeWadGuardian> AmmoTarget;
        TWeakObjectPtr<ANativeWadGuardian> LighthouseTarget;
        int32 LighthouseStairPoint = INDEX_NONE;
        TWeakObjectPtr<ANativeWadGuardian> RouteCombatTarget;
        double RouteCombatStarted = 0.0;
        bool bInventoryReview = false;
        bool bInventoryReload = false;
        bool bItemUseReview = false;
        bool bItemUseReload = false;
        int32 AndrewPlateIndex = 0;
        int32 AndrewPlateTurns = 0;
        int32 AndrewStartingGold = 0;
        bool bL22TrapReview = false;
        bool bLiftCheckpointReview = false;
        bool bLiftReload = false;
        bool bDoorReload = false;
        ETownReview TownReview = ETownReview::None;
        EUndeadReview UndeadReview = EUndeadReview::None;
        ETempleReview TempleReview = ETempleReview::None;
        const FCampaignRoute* CampaignRoute = nullptr;
        const FCampaignTurnIn* CampaignLaunch = nullptr;
        int32 CampaignPickupCount = INDEX_NONE;
        int32 CampaignGoldBeforeExit = 0;
        bool bCampaignActionStarted = false;
        bool bCampaignExitRecorded = false;
        bool bCampaignChain = false;
        bool bCampaignMovementSample = false;
        bool bCampaignArrivedInRun = false;
        bool bCombatBacking = false;
        double NextElymasCombatSample = 0.0;
        int32 BossPreparationPoint = 0;
        int32 CombatWeapon = INDEX_NONE;
        double CampaignStepTime = -1.0;
        double LastCampaignJumpTime = -1.0;
        double CampaignNextUseTime = 0.0;
        double CampaignJumpPeak = 0.0;
        double PreviousCampaignFrame = 0.0;
        double LongestWalkFrame = 0.0;
        int32 WalkFrameCount = 0;
        int32 WalkHitchCount = 0;
        int32 TemplePlate = 0;
        int32 PoisonBeforeTrap = 0;
        EL25Review L25Review = EL25Review::None;
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
        bool bFlightThrust = false;
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
            if (WalkFrameCount > 0)
            {
                UE_LOG(LogTemp, Display, TEXT("L20 movement timing: %d sampled frames, %d over 100 ms, longest %.1f ms."),
                    WalkFrameCount, WalkHitchCount, LongestWalkFrame * 1000.0);
            }
            if (Player) { SetCombatStrafe(*Player, 0); }
            if (Player && bWalking)
            {
                SendKey(*Player, EKeys::W, IE_Released);
            }
            if (Player && bCombatBacking)
            {
                SendKey(*Player, EKeys::S, IE_Released);
            }
            if (Player && HeldKey)
            {
                SendKey(*Player, HeldKey.GetValue(), IE_Released);
            }
            if (Player)
            {
                SetFlightThrust(*Player, false);
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

        void SetCombatStrafe(ANativeWadPlayer& Player, int32 Direction)
        {
            if (CombatStrafe == Direction) { return; }
            if (CombatStrafe != 0) { SendKey(Player, CombatStrafe < 0 ? EKeys::A : EKeys::D, IE_Released); }
            CombatStrafe = Direction;
            Player.GetCharacterMovement()->StopMovementImmediately();
            if (Direction != 0) { SendKey(Player, Direction < 0 ? EKeys::A : EKeys::D, IE_Pressed); }
        }

        bool SafeCombatGround(ANativeWadLevel& Level, ANativeWadPlayer& Player, const FVector& Candidate)
        {
            const FTransform Transform = Level.GetActorTransform();
            const FVector Local = Transform.InverseTransformPosition(Candidate);
            const int32 Sector = Level.FindWadSector(FVector2D(Local));
            const FVector Feet = Player.GetActorLocation() - FVector::UpVector * Player.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            const double Height = Transform.InverseTransformPosition(Feet).Z;
            FVector2D Limits;
            return Level.GetSectorSurfaceHeights(Sector, Limits) && FMath::Abs(Level.GetWalkingHeight(Sector) - Height) <= 24.0
                && Limits.Y - Limits.X >= 56.0;
        }

        void BeginCampaignMap(UWorld& Current, ANativeWadPlayer& Player)
        {
            Stop(Player);
            SetCombatStrafe(Player, 0);
            SetFlightThrust(Player, false);
            CombatWeapon = INDEX_NONE;
            NextElymasCombatSample = 0.0;
            CampaignObstacleDetour.Reset();
            CampaignObstacleAfter.Reset();
            WalkBlockingEnemy.Reset();
            bCampaignConcealmentVerified = false;
            bCampaignArrivalVerified = false;
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            if (ASceneCapture2D* Actor = Camera.Get())
            {
                Actor->Destroy();
            }
            Camera.Reset();
            CaptureFrames = 0;
            SourceWorld = &Current;
            RiverProgressTime = 0.0;
            NextPoint = 0;
        }

        bool WalkTo(ANativeWadPlayer& Player, const FVector2D& Target)
        {
            const FVector2D Offset = Target - FVector2D(Player.GetActorLocation());
            const double AcceptanceRadius = bRiverReview ? 16.0 : bRavineReview || bLiftCheckpointReview ? 12.0
                : bSecondMap || Stage == EStage::Arrival ? 14.0
                : CampaignRoute && (CampaignRoute->Map == 34 || CampaignRoute->Map == 60
                    || CampaignRoute->Map == 151 || CampaignRoute->Map == 152 || CampaignRoute->Map == 153
                    || CampaignRoute->Quest == 3) ? 12.0
                : CampaignRoute && (CampaignRoute->Map == 20 || CampaignRoute->Map == 64 || CampaignRoute->Map == 65
                    || CampaignRoute->Map == 70 || CampaignRoute->Map == 71) ? 8.0 : 5.0;
            if (Offset.SizeSquared() < FMath::Square(AcceptanceRadius))
            {
                Stop(Player);
                return true;
            }
            Player.GetController()->SetControlRotation(FVector(Offset, 0).Rotation());
            Walk(Player);
            return false;
        }

        void SetFlightThrust(ANativeWadPlayer& Player, bool bEnabled)
        {
            if (bFlightThrust != bEnabled)
            {
                SendKey(Player, EKeys::SpaceBar, bEnabled ? IE_Pressed : IE_Released);
                bFlightThrust = bEnabled;
            }
        }

        bool CaptureView(ANativeWadPlayer& Player, const TCHAR* Name, bool& bSaved, double BackUp = 0.0)
        {
            if (bWholeCampaign && FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
            {
                bSaved = true;
                return true;
            }
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

        void RecordTownCharacter(ANativeWadPlayer& Player)
        {
            if (Player.CanChooseWadClass())
            {
                Player.ChooseWadClass(Player.GetCharacterClass());
            }
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
            if ((bWholeCampaign || CampaignLaunch) && FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
            {
                bSaved = true;
                return true;
            }
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

        bool TickClassicHud(ANativeWadPlayer& Player)
        {
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                if (HeldKey.GetValue() == EKeys::NumPadSeven || HeldKey.GetValue() == EKeys::NumPadSix)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Released);
                }
                HeldKey.Reset();
            }
            APlayerController* Controller = Cast<APlayerController>(Player.GetController());
            ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
            if (!Hud || !Hud->HasClassicHud())
            {
                return Finish(&Player, TEXT("the native game did not load the classic HUD asset"));
            }
            const auto Click = [&](FVector2D Point)
            {
                int32 Width = 0;
                int32 Height = 0;
                Controller->GetViewportSize(Width, Height);
                const double Scale = FMath::Min(Width / 320.0, Height / 200.0);
                Hud->ClickHUD((FVector2D(Width, Height) - FVector2D(320, 200) * Scale) * 0.5 + Point * Scale);
            };
            const auto Capture = [&](const TCHAR* Name)
            {
                bool bSaved = false;
                return CaptureTownFrame(Name, bSaved) ? (bSaved ? 1 : -1) : 0;
            };
            switch (NextPoint)
            {
            case 0:
                if (Player.HasRestoredWadCheckpoint())
                {
                    const auto* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
                    if (!Travel || Player.GetItemCount(105) != 1 || Travel->Quest.GetWalletCopper() != 6043
                        || Travel->Quest.BankCopper != 9876)
                    {
                        return Finish(&Player, TEXT("the withdrawn five-coin pile or the remaining balances changed after reload"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the withdrawn coin pile and remaining finances after a process restart."));
                    return Finish(&Player);
                }
                if (Player.GetCarriedItems().ContainsByPredicate([](int32 Type) { return !NativeWadRules::IsRuneItem(Type); }) || Player.GetExperience() != 0)
                {
                    return Finish(&Player, TEXT("the classic HUD baseline needs a fresh character with only installed runes"));
                }
                if (const int32 Result = Capture(TEXT("exploring")); Result != 0)
                {
                    if (Result < 0)
                    {
                        return Finish(&Player, TEXT("could not capture the classic frame at the original spawn"));
                    }
                    Player.ReceiveEncounterHit(300);
                    Player.CollectItem(NativeWadRules::IronDagger);
                    Player.CollectItem(NativeWadRules::Berry);
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::NumPadSeven);
                    ++NextPoint;
                }
                break;
            case 1:
                if (!Player.IsInventoryOpen())
                {
                    return Finish(&Player, TEXT("the inventory key did not open the original inventory panel"));
                }
                Hud->ClickHUD(Hud->InventorySlotCenter(Player.GetCarriedItems().Find(NativeWadRules::IronDagger)));
                Hud->ClickHUD(Hud->InventorySlotCenter(INDEX_NONE));
                if (Player.GetReadyItem() != NativeWadRules::IronDagger)
                {
                    return Finish(&Player, TEXT("the classic inventory and ready hand hit targets could not equip the dagger"));
                }
                ++NextPoint;
                break;
            case 2:
                if (const int32 Result = Capture(TEXT("inventory")); Result != 0)
                {
                    if (Result < 0)
                    {
                        return Finish(&Player, TEXT("could not capture the classic inventory"));
                    }
                    constexpr int32 Helmet = 704;
                    if (!Player.CollectItem(Helmet))
                    {
                        return Finish(&Player, TEXT("could not supply the authored chain helmet"));
                    }
                    const int32 Slot = Player.GetCarriedItems().Find(Helmet);
                    Hud->ClickHUD(Hud->InventorySlotCenter(Slot));
                    Hud->ClickHUD(Hud->InventorySlotCenter(INDEX_NONE));
                    Click({265, 144});
                    if (!Player.IsWornInventorySlot(Slot))
                    {
                        return Finish(&Player, TEXT("the chain helmet was not equipped from the bag"));
                    }
                    ++NextPoint;
                }
                break;
            case 3:
                if (const int32 Result = Capture(TEXT("equipment")); Result != 0)
                {
                    constexpr int32 Helmet = 704;
                    const int32 Slot = Player.GetCarriedItems().Find(Helmet);
                    Click({250, 24});
                    if (Result < 0 || Player.IsWornInventorySlot(Slot))
                    {
                        return Finish(&Player, TEXT("the equipment form did not capture or remove the worn helmet"));
                    }
                    Click({265, 144});
                    Click({47, 161});
                    ++NextPoint;
                }
                break;
            case 4:
                if (const int32 Result = Capture(TEXT("statistics")); Result != 0)
                {
                    if (Result < 0)
                    {
                        return Finish(&Player, TEXT("could not capture the classic statistics"));
                    }
                    Click({309, 9});
                    if (Player.IsInventoryOpen())
                    {
                        return Finish(&Player, TEXT("the original close button did not dismiss the banner"));
                    }
                    Press(Player, EKeys::Tab);
                    ++NextPoint;
                }
                break;
            case 5:
                Click({272, 174});
                Click({289, 174});
                if (Player.GetRunes() != TEXT("45"))
                {
                    return Finish(&Player, TEXT("the original rune hit targets did not build the spell"));
                }
                ++NextPoint;
                break;
            case 6:
                if (const int32 Result = Capture(TEXT("runes")); Result != 0)
                {
                    const int32 Mana = Player.GetMana();
                    Click({249, 188});
                    if (Result < 0 || Player.GetMana() >= Mana || !Player.GetRunes().IsEmpty())
                    {
                        return Finish(&Player, TEXT("the original Cast button did not spend mana and clear the runes"));
                    }
                    auto* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
                    if (!Travel)
                    {
                        return Finish(&Player, TEXT("the classic finances review requires the player purse and bank balance"));
                    }
                    Travel->Quest.SetWalletCopper(6543);
                    Travel->Quest.BankCopper = 9876;
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::NumPadSix);
                    ++NextPoint;
                }
                break;
            case 7:
            {
                auto& Progress = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>()->Quest;
                Click({220, 49});
                Click({232, 49});
                if (!Player.IsInventoryOpen() || Player.GetItemCount(104) != 1 || Player.GetItemCount(105) != 0
                    || Progress.GetWalletCopper() != 6443)
                {
                    return Finish(&Player, TEXT("the finances shortcut or single-coin retrieval did not honor the mouse hand"));
                }
                Click({290, 48});
                Click({232, 49});
                if (Player.GetItemCount(104) != 0 || Player.GetItemCount(105) != 1 || Progress.GetWalletCopper() != 6043)
                {
                    return Finish(&Player, TEXT("the original finances targets did not return the coin and retrieve the five-coin pile"));
                }
                Click({290, 48});
                Click({232, 61});
                if (Player.GetItemCount(105) != 0 || Player.GetItemCount(103) != 0 || Progress.GetWalletCopper() != 6543
                    || Progress.BankCopper != 9876)
                {
                    return Finish(&Player, TEXT("the finances controls did not preserve the purse and bank balances"));
                }
                ++NextPoint;
                break;
            }
            case 8:
                if (const int32 Result = Capture(TEXT("finances")); Result != 0)
                {
                    if (Result < 0)
                    {
                        return Finish(&Player, TEXT("could not capture the original finances panel"));
                    }
                    Click({232, 49});
                    Click({46, 175});
                    if (Player.IsInventoryOpen() || Player.GetItemCount(105) != 1
                        || !Player.SaveWadCheckpoint(TEXT("NativeWadFinancesReview")))
                    {
                        return Finish(&Player, TEXT("the original finances banner button did not close and save the withdrawn pile"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the classic HUD at the L301 spawn, inventory, ready hand, equipment, statistics, rune casting, and finances."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickInventoryReview(ANativeWadPlayer& Player)
        {
            struct FPickup
            {
                const TCHAR* Tag;
                int32 Type;
                FVector2D Approach;
            };
            const FPickup Targets[] = {
                {TEXT("WAD Item Thing 211"), 245, {1628, 1156}},
                {TEXT("L30 Berry Pickup"), NativeWadRules::Berry, {1628, 1172}},
                {TEXT("WAD Item Thing 212"), 248, {1628, 1172}},
            };
            const auto FindPickup = [&](const TCHAR* Tag) -> ANativeWadPickup*
            {
                for (TActorIterator<ANativeWadPickup> It(Player.GetWorld()); It; ++It)
                {
                    if (It->ActorHasTag(FName(Tag)))
                    {
                        return *It;
                    }
                }
                return nullptr;
            };
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            APlayerController* Controller = Player.GetController<APlayerController>();
            ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
            const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
            if (!Hud || !Hud->HasClassicHud() || !Level || !Player.GetHealth())
            {
                return Finish(&Player, TEXT("the inventory review lost its player, level, or classic panel"));
            }
            const auto VerifyCollected = [&]
            {
                for (const FPickup& Target : Targets)
                {
                    if (Player.GetItemCount(Target.Type) != 1 || FindPickup(Target.Tag))
                    {
                        return false;
                    }
                }
                return Player.GetCarriedItems().Num() == UE_ARRAY_COUNT(Targets);
            };
            if (bInventoryReload)
            {
                if (!Player.HasRestoredWadCheckpoint() || !VerifyCollected() || Player.GetReadyItem() != NativeWadRules::Berry)
                {
                    return Finish(&Player, TEXT("the saved bag, ready hand, or collected world items did not restore"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the inventory and removed world pickups after a process restart."));
                return Finish(&Player);
            }
            const auto MouseY = [&](float Amount)
            {
                Controller->InputKey(FInputKeyEventArgs(nullptr, IPlatformInputDeviceMapper::Get().GetDefaultInputDevice(),
                    EKeys::MouseY, IE_Axis, Amount, false, FPlatformTime::Cycles64()));
            };
            const FVector2D Entrance[] = {{1256, 916}, {1268, 1192}, {1416, 1340}};
            if (NextPoint >= 3 && NextPoint < 6)
            {
                if (WalkTo(Player, Entrance[NextPoint - 3]))
                {
                    ++NextPoint;
                }
                return true;
            }
            if (NextPoint >= 9 && NextPoint < 12)
            {
                const FPickup& Target = Targets[NextPoint - 9];
                if (Player.GetItemCount(Target.Type) == 1)
                {
                    if (FindPickup(Target.Tag))
                    {
                        return Finish(&Player, TEXT("the collected item still exists in the world"));
                    }
                    ++NextPoint;
                    bCampaignActionStarted = false;
                }
                else if (!bCampaignActionStarted && WalkTo(Player, Target.Approach))
                {
                    const ANativeWadPickup* Item = FindPickup(Target.Tag);
                    if (!Item || Item->GetItemType() != Target.Type)
                    {
                        return Finish(&Player, TEXT("an authored pickup is missing from the level"));
                    }
                    Aim(Player, Item->GetActorLocation());
                    Press(Player, EKeys::E);
                    bCampaignActionStarted = true;
                }
                else if (bCampaignActionStarted)
                {
                    return Finish(&Player, TEXT("E did not move the aimed item into the bag"));
                }
                return true;
            }
            bool bSaved = false;
            switch (NextPoint)
            {
            case 0:
                if (!Player.GetCarriedItems().IsEmpty() || !FVector2D(Player.GetActorLocation()).Equals({1608, 432}, 5.0))
                {
                    return Finish(&Player, TEXT("the inventory review needs a fresh character at L30's spawn"));
                }
                Controller->SetControlRotation(FRotator::ZeroRotator);
                MouseY(18.0f);
                break;
            case 1:
                if (Controller->GetControlRotation().GetNormalized().Pitch <= 0.0)
                {
                    return Finish(&Player, TEXT("positive mouse Y did not pitch the game camera up"));
                }
                Controller->SetControlRotation(FRotator::ZeroRotator);
                MouseY(-18.0f);
                break;
            case 2:
                if (Controller->GetControlRotation().GetNormalized().Pitch >= 0.0)
                {
                    return Finish(&Player, TEXT("negative mouse Y did not pitch the game camera down"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified both directions of inverted mouse pitch through the gameplay input binding."));
                break;
            case 6:
                Aim(Player, {1440, 1340, 57});
                Press(Player, EKeys::E);
                break;
            case 7:
            {
                FVector2D Heights;
                if (!(*Level)->GetSectorSurfaceHeights(144, Heights) || Heights.Y < 100.0)
                {
                    return true;
                }
                break;
            }
            case 8:
                if (!WalkTo(Player, {1488, 1340}))
                {
                    return true;
                }
                break;
            case 12:
                if (!VerifyCollected() || Player.GetInventoryItemDescription(245).Contains(TEXT("Item 245")))
                {
                    return Finish(&Player, TEXT("the bag did not contain the three authored items and their descriptions"));
                }
                Controller->SetControlRotation(FRotator(0.0, 58.0, 0.0));
                Press(Player, EKeys::I);
                break;
            case 13:
                if (!Player.IsInventoryOpen() || Player.IsMouseLook())
                {
                    return Finish(&Player, TEXT("I did not open the interactive classic inventory"));
                }
                Hud->ClickHUD(Hud->InventorySlotCenter(1));
                Hud->ClickHUD(Hud->InventorySlotCenter(INDEX_NONE));
                Hud->ClickHUD(Hud->InventorySlotCenter(0));
                if (Player.GetReadyItem() != NativeWadRules::Berry || !Player.SaveWadCheckpoint(TEXT("NativeWadInventoryReview")))
                {
                    return Finish(&Player, TEXT("the classic inventory could not ready and save the world pickup"));
                }
                break;
            case 14:
                if (!CaptureTownFrame(TEXT("world-pickups"), bSaved))
                {
                    return true;
                }
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the inventory after the three world pickups"));
                }
                Player.CollectItem(245);
                if (Hud->InventorySlotCenter(0) != Hud->InventorySlotCenter(3))
                {
                    return Finish(&Player, TEXT("matching cherries were not displayed as a single stack"));
                }
                for (int32 Index = 0; Index < 48; ++Index)
                {
                    Player.CollectItem(NativeWadRules::IronDagger);
                }
                Press(Player, EKeys::PageDown);
                break;
            case 15:
                if (!bCampaignActionStarted)
                {
                    Hud->ClickHUD(Hud->InventorySlotCenter(Player.GetCarriedItems().Num() - 1));
                    bCampaignActionStarted = true;
                }
                if (Player.GetSelectedInventorySlot() != Player.GetCarriedItems().Num() - 1)
                {
                    return Finish(&Player, TEXT("Page Down did not load the second page's clickable cells"));
                }
                if (!CaptureTownFrame(TEXT("second-page"), bSaved))
                {
                    return true;
                }
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the second page of the classic inventory"));
                }
                Press(Player, EKeys::PageUp);
                break;
            case 16:
                Hud->ClickHUD(Hud->InventorySlotCenter(0));
                if (Player.GetSelectedInventorySlot() != 0)
                {
                    return Finish(&Player, TEXT("Page Up did not return to the first page"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified authored world pickups, descriptions, stacking, paging, and the ready hand."));
                return Finish(&Player);
            }
            ++NextPoint;
            return true;
        }

        bool TickItemUseReview(ANativeWadPlayer& Player)
        {
            constexpr int32 Cherries = 245;
            constexpr int32 IronShortSword = 203;
            constexpr int32 Armor[] = {704, 705, 707};
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            APlayerController* Controller = Player.GetController<APlayerController>();
            ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
            if (!Hud || !Player.GetHealth())
            {
                return Finish(&Player, TEXT("item-use review lost its player or inventory"));
            }
            const auto FindDrop = [&]() -> ANativeWadPickup*
            {
                for (TActorIterator<ANativeWadPickup> It(Player.GetWorld()); It; ++It)
                {
                    if (It->IsDropped() && It->GetItemType() == Cherries)
                    {
                        return *It;
                    }
                }
                return nullptr;
            };
            const auto Select = [&](int32 Type)
            {
                const int32 Slot = Player.GetCarriedItems().Find(Type);
                if (Slot == INDEX_NONE || !Player.IsInventoryOpen())
                {
                    return false;
                }
                Hud->ClickHUD(Hud->InventorySlotCenter(Slot));
                return Player.GetSelectedInventorySlot() == Slot;
            };
            const auto Ready = [&](int32 Type)
            {
                if (!Select(Type))
                {
                    return false;
                }
                Hud->ClickHUD(Hud->InventorySlotCenter(INDEX_NONE));
                return true;
            };
            if (bItemUseReload)
            {
                if (NextPoint == 0)
                {
                    const ANativeWadPickup* Drop = FindDrop();
                    if (!Player.HasRestoredWadCheckpoint() || !Drop || Player.GetItemCount(Cherries) != 1
                        || Player.GetItemCount(HealingPotion) != 0 || Player.GetEquippedWeapon() != IronShortSword
                        || Player.GetArmorRating() != 8)
                    {
                        return Finish(&Player, TEXT("the worn armor, ready sword, bag, or dropped item failed to restore"));
                    }
                    Aim(Player, Drop->GetActorLocation());
                    Press(Player, EKeys::E);
                    ++NextPoint;
                    return true;
                }
                if (FindDrop() || Player.GetItemCount(Cherries) != 2)
                {
                    return Finish(&Player, TEXT("E did not return exactly one dropped object to the saved stack"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the drop, worn armor, and ready sword restored, and E recovered one item."));
                return Finish(&Player);
            }
            switch (NextPoint)
            {
            case 0:
                Press(Player, EKeys::I);
                break;
            case 1:
                if (!Ready(Cherries) || Player.GetReadyItem() != Cherries)
                {
                    return Finish(&Player, TEXT("could not ready the original cherries"));
                }
                Press(Player, EKeys::I);
                break;
            case 2:
                FoodBeforeTravel = Player.GetFood();
                Press(Player, EKeys::LeftControl);
                break;
            case 3:
                if (Player.GetFood() != FoodBeforeTravel + 200 || Player.GetItemCount(Cherries) != 2)
                {
                    return Finish(&Player, TEXT("cherries did not apply their authored food effect and consume one"));
                }
                Press(Player, EKeys::I);
                break;
            case 4:
                if (!Ready(HealingPotion) || Player.GetReadyItem() != HealingPotion)
                {
                    return Finish(&Player, TEXT("could not ready the original healing potion"));
                }
                Press(Player, EKeys::I);
                break;
            case 5:
                HealthBeforeTravel = Player.GetHealth();
                Press(Player, EKeys::LeftControl);
                break;
            case 6:
                if (Player.GetHealth() != HealthBeforeTravel + 1000 || Player.GetItemCount(HealingPotion) != 0)
                {
                    return Finish(&Player, TEXT("the healing potion did not apply its authored health effect and consume one"));
                }
                Press(Player, EKeys::I);
                break;
            case 7:
                if (!Ready(IronShortSword))
                {
                    return Finish(&Player, TEXT("could not ready the authored iron short sword"));
                }
                for (const int32 Type : Armor)
                {
                    if (!Ready(Type))
                    {
                        return Finish(&Player, TEXT("could not equip an authored armor piece"));
                    }
                }
                if (Player.GetArmorRating() != 8 || Player.GetEquippedWeapon() != IronShortSword)
                {
                    return Finish(&Player, TEXT("the armor rating or the ready weapon did not match the original rules"));
                }
                HealthBeforeTravel = Player.GetHealth();
                Player.ReceivePhysicalHit(1000);
                if (HealthBeforeTravel - Player.GetHealth() != 960 || !Select(Cherries))
                {
                    return Finish(&Player, TEXT("the worn armor did not reduce ordinary melee damage"));
                }
                Controller->SetControlRotation(FRotator::ZeroRotator);
                Press(Player, EKeys::G);
                break;
            case 8:
                if (!FindDrop() || Player.GetItemCount(Cherries) != 1 || !Player.SaveWadCheckpoint(TEXT("NativeWadItemUseReview")))
                {
                    return Finish(&Player, TEXT("dropping a stack item did not leave exactly one in the world and one in the bag"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified original food and potion effects, melee equipment, armor mitigation, and dropping one item."));
                return Finish(&Player);
            }
            ++NextPoint;
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
                if (bSuccess && !Player.SaveWadCheckpoint(bFreshCampaign ? FreshCampaignTownSlot : TownReviewSlot))
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
                if (bWholeCampaign)
                {
                    CampaignLaunch = &CampaignQuests[0];
                    TownReview = ETownReview::None;
                    bFreshCampaign = false;
                    BeginCampaignMap(Current, Player);
                    return true;
                }
                return Finish(&Player);
            }
            return true;
        }

        bool TickTownScroll(ANativeWadPlayer& Player)
        {
            APlayerController* Controller = Player.GetController<APlayerController>();
            ANativeWadTownHUD* HUD = Controller ? Controller->GetHUD<ANativeWadTownHUD>() : nullptr;
            if (!HUD)
            {
                return Finish(&Player, TEXT("the scroll review requires the original town HUD"));
            }
            if (NextPoint == 0)
            {
                SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                Press(Player, EKeys::Q);
                ++NextPoint;
                return true;
            }
            const TCHAR* Frames[] = {TEXT("before"), TEXT("keyboard"), TEXT("wheel"), TEXT("arrow"), TEXT("restored")};
            const int32 Lines[] = {0, 1, 2, 1, 0};
            if (HUD->GetScrollLine() != Lines[NextPoint - 1])
            {
                return Finish(&Player, TEXT("a town scrolling input did not move the visible text by one line"));
            }
            bool bSaved = false;
            if (!CaptureTownFrame(Frames[NextPoint - 1], bSaved))
            {
                return true;
            }
            if (!bSaved)
            {
                return Finish(&Player, TEXT("the town scrolling frame was not written"));
            }
            switch (NextPoint++)
            {
            case 1: Press(Player, EKeys::Down); break;
            case 2: Press(Player, EKeys::MouseScrollDown); break;
            case 3:
            {
                int32 Width, Height;
                Controller->GetViewportSize(Width, Height);
                const double Scale = FMath::Min(Width / 320.0, Height / 200.0);
                const FVector2D Origin((Width - 320 * Scale) * 0.5, (Height - 200 * Scale) * 0.5);
                HUD->ClickAction(Origin + FVector2D(281, 24) * Scale);
                break;
            }
            case 4: Press(Player, EKeys::Up); break;
            default:
                UE_LOG(LogTemp, Display, TEXT("Verified original Town Hall briefing scrolls with keyboard, mouse wheel, and the on-screen arrow hit box."));
                return Finish(&Player);
            }
            return true;
        }

        bool PlaceReviewPlayer(ANativeWadLevel& Level, ANativeWadPlayer& Player, const FVector2D& Position)
        {
            const int32 Sector = Level.FindWadSector(Position);
            if (!Level.GetSectors().IsValidIndex(Sector))
            {
                return false;
            }
            Stop(Player);
            Player.SetActorLocation(FVector(Position, Level.GetWalkingHeight(Sector) + ANativeWadPlayer::CapsuleHalfHeight + 2.0),
                false, nullptr, ETeleportType::TeleportPhysics);
            Player.GetCharacterMovement()->StopMovementImmediately();
            return true;
        }

        bool TickL25Passages(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const FVector2D BridgeApproach(-180, 680);
            const FVector2D TimingPlate(-64, -462);
            const FVector2D FirstApproach(180, -2706);
            const FVector2D FirstPlate(249, -2706);
            const FVector2D ResetPlate(90, -2793);
            const FVector2D SecondPlate(-172, -2793);
            FVector2D Bridge, TimedGate, ExitGate, First, Second;
            if (!Level.GetSectorSurfaceHeights(158, Bridge) || !Level.GetSectorSurfaceHeights(100, TimedGate)
                || !Level.GetSectorSurfaceHeights(130, ExitGate) || !Level.GetSectorSurfaceHeights(15, First)
                || !Level.GetSectorSurfaceHeights(18, Second))
            {
                return Finish(&Player, TEXT("L25 lost an authored moving sector"));
            }
            const double Now = Current.GetTimeSeconds();
            bool bDone = false;
            switch (NextPoint)
            {
            case 0:
                if (!FVector2D(Player.GetActorLocation()).Equals({298, 1636}, 3.0))
                {
                    return Finish(&Player, TEXT("the L25 review requires the original spawn"));
                }
                if (Now >= 1.2)
                {
                    if (First.X != 132.0 || Second.X != 132.0 || Bridge.X != -72.0 || TimedGate.Y != -44.0
                        || ExitGate.Y != 136.0 || !PlaceReviewPlayer(Level, Player, BridgeApproach))
                    {
                        return Finish(&Player, TEXT("L25 startup did not initialize the original passages and plates"));
                    }
                    bDone = true;
                }
                break;
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-180, 728, -168});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    bDone = true;
                }
                break;
            case 2:
                if (Bridge.X == -208.0)
                {
                    bool bSaved = false;
                    bDone = CaptureView(Player, TEXT("lowered-bridge"), bSaved);
                    if (bDone && !bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the lowered L25 bridge"));
                    }
                }
                break;
            case 3:
                bDone = WalkTo(Player, {-180, 775});
                if (bDone && !PlaceReviewPlayer(Level, Player, {-64, -408}))
                {
                    return Finish(&Player, TEXT("could not reach the next fixture after crossing L25's bridge"));
                }
                break;
            case 4:
                if (Bridge.X == -72.0)
                {
                    if (!FMath::IsNearlyEqual(Now - StageTime, 9.0, 0.18))
                    {
                        return Finish(&Player, TEXT("L25's bridge did not restore after the authored delay"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified E on L25 line 852 lowers bridge 158 to Z-208, allows crossing and restores it to Z-72."));
                    bDone = true;
                }
                break;
            case 5:
                bDone = WalkTo(Player, TimingPlate);
                if (bDone)
                {
                    StageTime = Now;
                }
                break;
            case 6:
                if (TimedGate.Y == 72.0)
                {
                    Aim(Player, {-64, -831, 5});
                    bool bSaved = false;
                    bDone = CaptureView(Player, TEXT("timed-gate"), bSaved);
                    if (bDone && !bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture L25's timed gate"));
                    }
                }
                break;
            case 7:
                bDone = WalkTo(Player, {-64, -896});
                if (bDone && (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != 99
                    || !PlaceReviewPlayer(Level, Player, FirstApproach)))
                {
                    return Finish(&Player, TEXT("the player did not cross L25's timed gate"));
                }
                break;
            case 8:
                if (TimedGate.Y == -44.0)
                {
                    if (!FMath::IsNearlyEqual(Now - StageTime, 9.0, 0.35))
                    {
                        return Finish(&Player, TEXT("L25's pressure-plate gate did not finish the authored cycle"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L25 plate 105 opens gate 100 to Z72 and closes it after the timed crossing."));
                    bDone = true;
                }
                break;
            case 9:
                bDone = WalkTo(Player, FirstPlate) && First.X == 112.0;
                break;
            case 10:
                bDone = WalkTo(Player, FirstApproach);
                break;
            case 11:
                if (WalkTo(Player, FirstPlate))
                {
                    FWadLevelCheckpoint State;
                    if (!Level.WriteCheckpoint(State) || State.Variables[1] != 1 || ExitGate.Y != 136.0
                        || !PlaceReviewPlayer(Level, Player, {90, -2724}))
                    {
                        return Finish(&Player, TEXT("reusing the same L25 plate incorrectly advanced the exit"));
                    }
                    bDone = true;
                }
                break;
            case 12:
                if (WalkTo(Player, ResetPlate) && First.X == 132.0)
                {
                    FWadLevelCheckpoint State;
                    if (!Level.WriteCheckpoint(State) || State.Variables[1] != 0
                        || !PlaceReviewPlayer(Level, Player, FirstApproach))
                    {
                        return Finish(&Player, TEXT("L25's reset plate lost the source counter reset"));
                    }
                    bDone = true;
                }
                break;
            case 13:
                if (WalkTo(Player, FirstPlate) && First.X == 112.0)
                {
                    if (!PlaceReviewPlayer(Level, Player, {-108, -2792}))
                    {
                        return Finish(&Player, TEXT("the second L25 plate approach is missing"));
                    }
                    bDone = true;
                }
                break;
            case 14:
                if (WalkTo(Player, SecondPlate) && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    FWadLevelCheckpoint State;
                    if (!Level.WriteCheckpoint(State) || State.Variables[1] != 2 || !State.PendingPickupGenerators.Contains(0)
                        || !Player.SaveWadCheckpoint(L25KeySlot))
                    {
                        return Finish(&Player, TEXT("the paired L25 plates did not start the exit gate and pending key checkpoint"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L25 ignores repeat entry, honors the reset plate, and starts generator 0 only after both plates."));
                    bDone = true;
                }
                break;
            case 15:
                if (ExitGate.Y == 240.0 && Second.X == 112.0)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L25's paired plates raised exit gate 130 to Z240."));
                    return Finish(&Player);
                }
                break;
            default:
                return Finish(&Player, TEXT("the L25 passage review reached an invalid step"));
            }
            if (bDone)
            {
                ++NextPoint;
                RiverProgressTime = FPlatformTime::Seconds();
            }
            return true;
        }

        bool TickL25Key(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            using NativeWadRules::ChurchKey;
            const auto* KeyDoor = Level.GetDoors().FindByPredicate([](const FAAWadNativeDoor& Entry) { return Entry.Sector == 148; });
            const FVector DoorUse(9, -3290, 17);
            if (!KeyDoor || KeyDoor->RequiredItem != ChurchKey)
            {
                return Finish(&Player, TEXT("L25's door is missing its authored key requirement"));
            }
            if (!Player.HasRestoredWadCheckpoint())
            {
                return true;
            }
            FVector2D Heights;
            if (!Level.GetSectorSurfaceHeights(148, Heights))
            {
                return Finish(&Player, TEXT("L25's key door lost its surfaces"));
            }
            if (L25Review == EL25Review::DoorReload)
            {
                if (NextPoint == 0)
                {
                    FWadLevelCheckpoint State;
                    if (!Level.WriteCheckpoint(State) || !State.UnlockedItemDoors.Contains(148)
                        || !State.FiredPickupGenerators.Contains(0) || Player.GetItemCount(ChurchKey) != 0
                        || State.RemainingPickups.Contains(FName(TEXT("L25 Key Generator 0"))))
                    {
                        return Finish(&Player, TEXT("restart lost the unlocked L25 door or respawned its key"));
                    }
                    ++NextPoint;
                    return true;
                }
                if (NextPoint == 1 && Heights.Y == Heights.X)
                {
                    Aim(Player, DoorUse);
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                if (NextPoint == 2 && Heights.Y == KeyDoor->OpenZ)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L25 door 148 reopened without a second key after a fresh process."));
                    return Finish(&Player);
                }
                return true;
            }

            const TActorIterator<ANativeWadPickup> Pickup(Level.GetWorld());
            if (NextPoint == 0)
            {
                FWadLevelCheckpoint State;
                if (!Level.WriteCheckpoint(State) || !State.PendingPickupGenerators.Contains(0)
                    || !Pickup || Pickup->GetItemType() != ChurchKey || Pickup->IsAvailable())
                {
                    return Finish(&Player, TEXT("the fresh process did not restore the pending one-shot generator"));
                }
                if (!PlaceReviewPlayer(Level, Player, {40, -3290}))
                {
                    return Finish(&Player, TEXT("the L25 key door approach is missing"));
                }
                ++NextPoint;
            }
            else if (NextPoint == 1 && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                Aim(Player, DoorUse);
                Press(Player, EKeys::E);
                ++NextPoint;
            }
            else if (NextPoint == 2)
            {
                if (Heights.Y != Heights.X || Player.GetInteractionMessage() != TEXT("Door requires key to be opened."))
                {
                    return Finish(&Player, TEXT("L25 did not refuse the door without its key"));
                }
                if (!PlaceReviewPlayer(Level, Player, {-8, -2700}))
                {
                    return Finish(&Player, TEXT("the L25 key approach is missing"));
                }
                ++NextPoint;
            }
            else if (NextPoint == 3 && Pickup && Pickup->IsAvailable())
            {
                Aim(Player, Pickup->GetActorLocation());
                bool bSaved = false;
                if (CaptureView(Player, TEXT("generated-key"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the generated L25 key"));
                    }
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
            }
            else if (NextPoint == 4 && Player.GetItemCount(ChurchKey) == 1)
            {
                if (!PlaceReviewPlayer(Level, Player, {40, -3290}))
                {
                    return Finish(&Player, TEXT("could not return to the L25 key door approach"));
                }
                ++NextPoint;
            }
            else if (NextPoint == 5 && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                Aim(Player, DoorUse);
                Press(Player, EKeys::E);
                ++NextPoint;
            }
            else if (NextPoint == 6 && Heights.Y == KeyDoor->OpenZ)
            {
                FWadLevelCheckpoint State;
                if (Player.GetItemCount(ChurchKey) != 0 || !Level.WriteCheckpoint(State)
                    || !State.UnlockedItemDoors.Contains(148) || !State.FiredPickupGenerators.Contains(0)
                    || !Player.SaveWadCheckpoint(L25DoorSlot))
                {
                    return Finish(&Player, TEXT("opening L25 door 148 did not consume and persist its authored key"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the saved generator released the authored L25 key once and E consumed it to unlock door 148."));
                ++NextPoint;
            }
            else if (NextPoint == 7)
            {
                Aim(Player, DoorUse);
                bool bSaved = false;
                if (CaptureView(Player, TEXT("unlocked-door"), bSaved))
                {
                    return bSaved ? Finish(&Player) : Finish(&Player, TEXT("could not capture the unlocked L25 door"));
                }
            }
            return true;
        }

        bool TickChurchKnights(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            ANativeWadGuardian* Foot = nullptr;
            ANativeWadGuardian* Mounted = nullptr;
            int32 Count = 0;
            for (TActorIterator<ANativeWadGuardian> Knight(&Current); Knight; ++Knight)
            {
                ++Count;
                if (Knight->ActorHasTag(TEXT("WAD Knight Thing 6"))) { Foot = *Knight; }
                if (Knight->ActorHasTag(TEXT("WAD Knight Thing 3"))) { Mounted = *Knight; }
            }
            if (Count != 9 || !Foot || !Mounted)
            {
                return Finish(&Player, TEXT("L25 is missing one of the nine authored knights"));
            }
            if (NextPoint == 0)
            {
                if (!FVector2D(Player.GetActorLocation()).Equals({298, 1636}, 3.0)
                    || Foot->GetEncounterHealth() != 3000 || Mounted->GetEncounterHealth() != 3000
                    || !PlaceReviewPlayer(Level, Player, {730, 123}))
                {
                    return Finish(&Player, TEXT("the L25 knight review requires the original spawn and source health"));
                }
                HealthBeforeTravel = Player.GetHealth();
                ++NextPoint;
                return true;
            }
            if (NextPoint == 1 || NextPoint == 2)
            {
                const bool bMounted = NextPoint == 1;
                ANativeWadGuardian& Target = bMounted ? *Mounted : *Foot;
                if (!bCapturedEncounter)
                {
                    Aim(Player, Target.GetActorLocation());
                    bool bSaved = false;
                    if (CaptureView(Player, bMounted ? TEXT("mounted-knight") : TEXT("crossbowman"), bSaved))
                    {
                        if (!bSaved)
                        {
                            return Finish(&Player, TEXT("could not capture the authored knight"));
                        }
                        bCapturedEncounter = true;
                    }
                }
                const int32 Damage = HealthBeforeTravel - Player.GetHealth();
                if (Damage > NativeWadRules::ChurchBoltDamage)
                {
                    return Finish(&Player, TEXT("the isolated knight volley applied unexpected damage"));
                }
                if (bCapturedEncounter && Target.GetBoltsFired() > 0 && Damage == NativeWadRules::ChurchBoltDamage)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L25's %s landed its authored 300-damage bolt."), *Target.GetEncounterName());
                    if (bMounted)
                    {
                        if (!PlaceReviewPlayer(Level, Player, {-589, -700}))
                        {
                            return Finish(&Player, TEXT("the L25 crossbowman approach is missing"));
                        }
                        bCapturedEncounter = false;
                        HealthBeforeTravel = Player.GetHealth();
                        ++NextPoint;
                    }
                    else
                    {
                        const FVector2D ClosePosition = FVector2D(Foot->GetActorLocation()) + FVector2D(44, 0);
                        if (!PlaceReviewPlayer(Level, Player, ClosePosition))
                        {
                            return Finish(&Player, TEXT("the L25 melee approach is missing"));
                        }
                        ExperienceBeforeTravel = Player.GetExperience();
                        ++NextPoint;
                    }
                }
                return true;
            }
            if (Player.GetExperience() > ExperienceBeforeTravel && Foot->GetEncounterHealth() < 3000)
            {
                UE_LOG(LogTemp, Display, TEXT("Verified player dagger input damages the authored L25 crossbowman and awards experience."));
                return Finish(&Player);
            }
            const FVector Offset = Foot->GetActorLocation() - Player.GetActorLocation();
            Aim(Player, Foot->GetActorLocation());
            if (Offset.Size2D() > 60.0)
            {
                Walk(Player);
            }
            else
            {
                Stop(Player);
                if (Current.GetTimeSeconds() >= StageTime)
                {
                    Press(Player, EKeys::LeftControl);
                    StageTime = Current.GetTimeSeconds() + 0.75;
                }
            }
            return true;
        }

        bool BeginChurchExit(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const auto* Fixture = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(L25DoorSlot, 0));
            const double ArrivalHour = Level.GetTimeOfDayHours();
            if (!Fixture || Fixture->LevelPackage != TEXT("/Game/Maps/L25") || !Fixture->Level.UnlockedItemDoors.Contains(148)
                || !Level.RestoreCheckpoint(Fixture->Level) || !PlaceReviewPlayer(Level, Player, FVector2D(Fixture->Position)))
            {
                return Finish(&Player, TEXT("the church exit review needs the L25 key-door checkpoint from ChurchKeyReload"));
            }
            Level.SetTimeOfDayHours(ArrivalHour);
            TownReview = ETownReview::None;
            NextPoint = 0;
            CaptureFrames = 0;
            UE_LOG(LogTemp, Display, TEXT("Loaded the verified L25 key-door state for the final walk, keeping the active quest from Town Hall."));
            return true;
        }

        bool TickChurchExit(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            const FString Map = Current.RemovePIEPrefix(Current.GetMapName());
            if (!Travel || Travel->Quest.Active != 1)
            {
                return Finish(&Player, TEXT("the Church of Andrew route lost the active quest from town"));
            }
            if (Map == TEXT("L25"))
            {
                if (NextPoint == 2 && Travel->Quest.ResumeMaps.FindRef(1) == 20)
                {
                    return true;
                }
                const auto* Link = Level.GetLevelLinks().FindByPredicate([](const FAAWadNativeLevelLink& Entry)
                    { return Entry.SourceThing == 85 && Entry.Sector == 149; });
                const auto* KeyDoor = Level.GetDoors().FindByPredicate([](const FAAWadNativeDoor& Entry) { return Entry.Sector == 148; });
                FVector2D Heights = FVector2D::ZeroVector;
                if (!Link || Link->Destination.ToSoftObjectPath().GetLongPackageName() != TEXT("/Game/Maps/L20")
                    || !KeyDoor || !Level.GetSectorSurfaceHeights(148, Heights) || Travel->Quest.ResumeMaps.FindRef(1) != 25)
                {
                    return Finish(&Player, TEXT("L25 lost its authored link or key door"));
                }
                if (NextPoint == 0 && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {9, -3290, 17});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                    return true;
                }
                if (NextPoint == 1 && Heights.Y == KeyDoor->OpenZ)
                {
                    Aim(Player, {-182, -3290, 17});
                    bool bSaved = false;
                    if (CaptureView(Player, TEXT("l25-exit-passage"), bSaved))
                    {
                        if (!bSaved)
                        {
                            return Finish(&Player, TEXT("could not capture the L25 exit passage"));
                        }
                        ++NextPoint;
                    }
                    return true;
                }
                if (NextPoint == 2)
                {
                    RecordTownCharacter(Player);
                    HourBeforeTravel = Level.GetTimeOfDayHours();
                    if (WalkTo(Player, {-224, -3290}))
                    {
                        return Finish(&Player, TEXT("walking into L25 sector 149 did not travel to L20"));
                    }
                }
                return true;
            }
            const FWadLevelCheckpoint* L25 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L25"));
            if (Map != TEXT("L20") || !FVector2D(Player.GetActorLocation()).Equals({-1084, 4630}, 3.0)
                || Travel->Quest.ResumeMaps.FindRef(1) != 20 || !MatchesTownCharacter(Player)
                || !FMath::IsNearlyEqual(Level.GetTimeOfDayHours(), HourBeforeTravel, 0.02)
                || !L25 || !L25->UnlockedItemDoors.Contains(148) || L25->Encounters.Num() != 9)
            {
                return Finish(&Player, TEXT("L20 did not receive the original spawn, character, clock, or archived church"));
            }
            if (!bArrivalCaptured)
            {
                bArrivalCaptured = true;
                bWalking = false;
                Camera.Reset();
            }
            bool bSaved = false;
            if (CaptureView(Player, TEXT("l20-arrival"), bSaved))
            {
                if (!bSaved || !Player.SaveWadCheckpoint(L20ReviewSlot))
                {
                    return Finish(&Player, TEXT("could not capture and save the L20 arrival"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified W through L25's original exit takes the active quest to the L20 spawn and archives L25."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickL20Reload(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            constexpr int32 TimedFloor = 16;
            const FVector2D Approach(-624, 2400);
            const FVector2D Plate(-740, 2400);
            const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            FVector2D Heights;
            if (!Travel || !Level.GetSectorSurfaceHeights(TimedFloor, Heights))
            {
                return Finish(&Player, TEXT("L20 lost its checkpoint or its authored timed floor"));
            }
            if (!Player.HasRestoredWadCheckpoint())
            {
                return true;
            }
            if (NextPoint == 0)
            {
                const FWadLevelCheckpoint* L25 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L25"));
                if (Travel->Quest.Active != 1 || Travel->Quest.ResumeMaps.FindRef(1) != 20
                    || !FVector2D(Player.GetActorLocation()).Equals({-1084, 4630}, 3.0)
                    || !L25 || !L25->UnlockedItemDoors.Contains(148) || L25->Encounters.Num() != 9
                    || !PlaceReviewPlayer(Level, Player, Approach))
                {
                    return Finish(&Player, TEXT("restart lost the L20 resume point or archived L25 state"));
                }
                ++NextPoint;
                UE_LOG(LogTemp, Display, TEXT("Verified the L20 checkpoint restored the active quest, original spawn, and archived L25."));
                return true;
            }
            if (NextPoint == 1)
            {
                if (!bTrailActionPressed && Level.FindActivitySector(FVector2D(Player.GetActorLocation())) == TimedFloor)
                {
                    StageTime = Current.GetTimeSeconds();
                    bTrailActionPressed = true;
                }
                if (WalkTo(Player, Plate))
                {
                    ++NextPoint;
                }
            }
            else if (NextPoint == 2 && WalkTo(Player, Approach))
            {
                ++NextPoint;
            }
            else if (NextPoint == 3 && Heights.X == -300.0)
            {
                Aim(Player, {-754, 2400, -142});
                bool bSaved = false;
                if (CaptureView(Player, TEXT("l20-lowered-floor"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture L20's lowered floor"));
                    }
                    ++NextPoint;
                }
            }
            else if (NextPoint == 4 && Heights.X == -16.0)
            {
                if (!bTrailActionPressed || !FMath::IsNearlyEqual(Current.GetTimeSeconds() - StageTime, 9.0, 0.25))
                {
                    return Finish(&Player, TEXT("L20's floor did not use the authored nine-second cycle"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified L20 activity 1 lowers floor 16 to Z-300 and returns it after its authored cycle."));
                if (!PlaceReviewPlayer(Level, Player, {-1400, 2714}))
                {
                    return Finish(&Player, TEXT("the authored L20 bolt plate approach is missing"));
                }
                TrapMissilesBefore = Level.GetScriptMissileCount(NativeWadRules::PiercingCrossbowBolt);
                ++NextPoint;
            }
            else if (NextPoint == 5 && WalkTo(Player, {-1400, 2660}))
            {
                if (Level.GetScriptMissileCount(NativeWadRules::PiercingCrossbowBolt) != TrapMissilesBefore + 1)
                {
                    return Finish(&Player, TEXT("L20's plate did not launch its authored piercing bolt"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified L20 sector 41 launches the original piercing bolt through activity 7."));
                return Finish(&Player);
            }
            return true;
        }


        bool TickChurchScenery(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const bool bL20 = L25Review == EL25Review::L20Scenery;
            const FVector2D Start = bL20 ? FVector2D(-1910, 3311) : FVector2D(1000, 408);
            const FVector2D Target = bL20 ? FVector2D(-1799, 3311) : FVector2D(1184, 408);
            const FVector2D CameraPoint = bL20 ? FVector2D(-2170, 1540) : FVector2D(960, -210);
            const double Now = Current.GetTimeSeconds();
            bool bSaved = false;
            switch (NextPoint)
            {
            case 0:
            {
                int32 Count = 0;
                const ANativeWadScenery* Fixture = nullptr;
                const FName Tag(bL20 ? TEXT("WAD Scenery Thing 80") : TEXT("WAD Scenery Thing 10"));
                for (TActorIterator<ANativeWadScenery> Prop(&Current); Prop; ++Prop)
                {
                    ++Count;
                    if (Prop->ActorHasTag(Tag))
                    {
                        Fixture = *Prop;
                    }
                }
                if (!Fixture || Count != (bL20 ? 82 : 102) || !PlaceReviewPlayer(Level, Player, Start))
                {
                    return Finish(&Player, TEXT("scenery is missing from its authored map"));
                }
                // Match the native bolt sweep. Aim at the rock's height to verify passability through its sprite.
                const double TraceZ = Fixture->GetActorLocation().Z + (bL20 ? 7.0 : 38.0);
                const FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadSceneryReview), true, &Player);
                FHitResult Hit;
                const bool bHit = Current.SweepSingleByChannel(Hit, FVector(Start, TraceZ), FVector(Target, TraceZ),
                    FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(5.0f), Query);
                const bool bExpectedHit = bL20 ? !bHit : bHit && Hit.GetActor() == Fixture;
                if (!bExpectedHit)
                {
                    return Finish(&Player, TEXT("the projectile collision channel disagrees with the scenery's passability"));
                }
                ++NextPoint;
                break;
            }
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 2:
                if (WalkTo(Player, Target) || Now - StageTime >= 2.0)
                {
                    Stop(Player);
                    const double ExpectedX = bL20 ? Target.X : Target.X - 20 - ANativeWadPlayer::CapsuleRadius;
                    if (!FMath::IsNearlyEqual(Player.GetActorLocation().X, ExpectedX, bL20 ? 6.0 : 3.0))
                    {
                        return Finish(&Player, TEXT("movement did not match the authored scenery collision"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("%s at X%.1f."),
                        bL20 ? TEXT("Verified W crosses L20's passable rock Thing 80")
                             : TEXT("Verified W stops at L25's solid tree Thing 10 and its source radius"),
                        Player.GetActorLocation().X);
                    if (!PlaceReviewPlayer(Level, Player, CameraPoint))
                    {
                        return Finish(&Player, TEXT("could not reach the scenery camera position"));
                    }
                    const FVector View = Player.GetPawnViewLocation();
                    Aim(Player, View + (bL20 ? FVector(270, 25, 10) : FVector(-230, -228, 0)));
                    ++NextPoint;
                }
                break;
            case 3:
                if (CaptureView(Player, bL20 ? TEXT("column-court") : TEXT("churchyard"), bSaved))
                {
                    const FVector2D SecondView = bL20 ? FVector2D(-1905, 2215) : FVector2D(910, 665);
                    if (!bSaved || !PlaceReviewPlayer(Level, Player, SecondView))
                    {
                        return Finish(&Player, TEXT("could not capture the authored scenery"));
                    }
                    Aim(Player, Player.GetPawnViewLocation() + (bL20 ? FVector(150, 260, 4) : FVector(-200, -260, 14)));
                    ++NextPoint;
                }
                break;
            case 4:
                if (CaptureView(Player, bL20 ? TEXT("grove") : TEXT("church-approach"), bSaved))
                {
                    return Finish(&Player, bSaved ? nullptr : TEXT("could not capture the second view of the authored scenery"));
                }
                break;
            }
            return true;
        }

        bool TickL20Exit(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            constexpr int32 Gate = 87;
            const FVector2D Spawn(-3076, -3);
            const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel || Travel->Quest.Active != 1)
            {
                return Finish(&Player, TEXT("the L20 exit review needs the active church quest"));
            }
            bool bSaved = false;
            if (Current.GetMapName().EndsWith(TEXT("L60")))
            {
                const FWadLevelCheckpoint* L20 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L20"));
                double ArrivalHour = Level.GetTimeOfDayHours();
                if (!Level.HasDaySky() && !LexTryParseString(ArrivalHour, Current.URL.GetOption(TEXT("WadHour="), TEXT("-1"))))
                {
                    return Finish(&Player, TEXT("L60 could not read the travel clock"));
                }
                if (Travel->Quest.ResumeMaps.FindRef(1) != 60 || !FVector2D(Player.GetActorLocation()).Equals(Spawn, 3.0)
                    || !MatchesTownCharacter(Player) || !FMath::IsNearlyEqual(ArrivalHour, HourBeforeTravel, 0.02)
                    || !L20 || L20->SectorHeights.FindRef(Gate).Y != 190.0 || L20->Encounters.Num() != 16
                    || !Travel->VisitedLevels.Contains(TEXT("/Game/Maps/L25")))
                {
                    return Finish(&Player, TEXT("L60 did not receive the authored spawn, character, clock or L20 gate state"));
                }
                if (!bArrivalCaptured)
                {
                    bArrivalCaptured = true;
                    bWalking = false;
                    Camera.Reset();
                }
                if (CaptureView(Player, TEXT("l60-arrival"), bSaved))
                {
                    if (!bSaved || !Player.SaveWadCheckpoint(L60ReviewSlot))
                    {
                        return Finish(&Player, TEXT("could not capture and save the L60 arrival"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L20 Thing 138 travels to L60 and saves the active quest and opened gate."));
                    return Finish(&Player);
                }
                return true;
            }

            const int32 ResumeMap = Travel->Quest.ResumeMaps.FindRef(1);
            if (NextPoint == 8 && ResumeMap == 60)
            {
                return true; // The travel subsystem has advanced while Unreal is still in the outgoing world.
            }
            const auto* Link = Level.GetLevelLinks().FindByPredicate([](const FAAWadNativeLevelLink& Entry)
                { return Entry.SourceThing == 138 && Entry.Sector == 131; });
            FVector2D GateHeights = FVector2D::ZeroVector;
            if (ResumeMap != 20 || !Player.HasRestoredWadCheckpoint()
                || !Link || Link->Destination.ToSoftObjectPath().GetLongPackageName() != TEXT("/Game/Maps/L60")
                || !Level.GetSectorSurfaceHeights(Gate, GateHeights))
            {
                return Finish(&Player, TEXT("the L20 exit review needs ChurchL20's checkpoint and authored link"));
            }
            switch (NextPoint)
            {
            case 0:
                if (GateHeights != FVector2D(112, 112) || !PlaceReviewPlayer(Level, Player, {0, 1640}))
                {
                    return Finish(&Player, TEXT("L20's exit gate did not start closed or the plate approach was missing"));
                }
                ++NextPoint;
                break;
            case 1:
                if (WalkTo(Player, {0, 1690}))
                {
                    if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != 108
                        || !Player.GetInteractionMessage().Contains(TEXT("The exit has now been opened")))
                    {
                        return Finish(&Player, TEXT("the L20 plate did not run authored activity 12"));
                    }
                    StageTime = Current.GetTimeSeconds();
                    ++NextPoint;
                }
                break;
            case 2:
                if (GateHeights.Y == 190.0)
                {
                    const double Elapsed = Current.GetTimeSeconds() - StageTime;
                    if (!FMath::IsNearlyEqual(Elapsed, 3.0, 0.25) || !PlaceReviewPlayer(Level, Player, {-171, 1792}))
                    {
                        return Finish(&Player, TEXT("L20 gate 87 opened at the wrong speed or its approach was missing"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L20 activity 12 raised exit gate 87 from Z112 to Z190 in %.2fs."), Elapsed);
                    Aim(Player, {140, 1792, Player.GetPawnViewLocation().Z});
                    ++NextPoint;
                }
                break;
            case 3:
                if (CaptureView(Player, TEXT("l20-open-exit"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the open L20 exit gate"));
                    }
                    ++NextPoint;
                }
                break;
            case 4:
                if (WalkTo(Player, {30, 1792}))
                {
                    if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != 88)
                    {
                        return Finish(&Player, TEXT("the player did not cross the open L20 gate"));
                    }
                    ++NextPoint;
                }
                break;
            case 5:
                if (WalkTo(Player, {196, 1792}))
                {
                    ++NextPoint;
                }
                break;
            case 6:
                if (WalkTo(Player, {196, 1888}))
                {
                    ++NextPoint;
                }
                break;
            case 7:
                if (WalkTo(Player, {212, 1903}))
                {
                    ++NextPoint;
                }
                break;
            case 8:
                RecordTownCharacter(Player);
                HourBeforeTravel = Level.GetTimeOfDayHours();
                if (WalkTo(Player, {212, 1946}))
                {
                    return Finish(&Player, TEXT("walking into L20 sector 131 did not travel to L60"));
                }
                break;
            }
            return true;
        }

        bool TickL60Reload(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            if (NextPoint == 0)
            {
                const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(L60ReviewSlot, 0));
                const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
                const FWadLevelCheckpoint* L20 = Travel ? Travel->VisitedLevels.Find(TEXT("/Game/Maps/L20")) : nullptr;
                int32 SceneryCount = 0;
                for (TActorIterator<ANativeWadScenery> Prop(&Current); Prop; ++Prop)
                {
                    ++SceneryCount;
                }
                if (!Save || !Player.HasRestoredWadCheckpoint() || !Travel || Travel->Quest.Active != 1
                    || Travel->Quest.ResumeMaps.FindRef(1) != 60 || !L20 || L20->SectorHeights.FindRef(87).Y != 190.0
                    || !FVector2D(Player.GetActorLocation()).Equals({-3076, -3}, 3.0)
                    || Save->Level.Encounters.Num() != 25 || SceneryCount != 84)
                {
                    return Finish(&Player, TEXT("the L60 checkpoint did not restore its quest, player, scenery and archived L20 gate"));
                }
                const FURL Options(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
                const FString Items = FString::JoinBy(Player.GetCarriedItems(), TEXT(","), [](int32 Type) { return LexToString(Type); });
                if (Items != Options.GetOption(TEXT("WadItems="), TEXT(""))
                    || Player.GetHealth() != FCString::Atoi(Options.GetOption(TEXT("WadHealth="), TEXT("-1")))
                    || Player.GetExperience() != FCString::Atoi(Options.GetOption(TEXT("WadXP="), TEXT("-1"))))
                {
                    return Finish(&Player, TEXT("the L60 checkpoint changed the character's items, health or experience"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the L60 checkpoint restored the quest, authored spawn, encounters, scenery and archived L20 gate."));
                if (!PlaceReviewPlayer(Level, Player, {-1400, 0}))
                {
                    return Finish(&Player, TEXT("the original L60 column hall was missing"));
                }
                Aim(Player, {-800, 0, Player.GetPawnViewLocation().Z + 20});
                ++NextPoint;
            }
            bool bSaved = false;
            return !CaptureView(Player, TEXT("l60-column-hall"), bSaved)
                || Finish(&Player, bSaved ? nullptr : TEXT("could not capture the L60 column hall"));
        }

        bool TickL60Puzzles(UWorld& Current, ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            struct FPlate
            {
                int32 Sector, Object, Turns;
                FVector2D Approach, Inside;
            };
            const FPlate Plates[] = {
                {231, 297, 2, {2191, 1056}, {2234, 1056}},
                {230, 293, 1, {1775, 1696}, {1818, 1696}},
                {233, 294, 2, {2447, 1600}, {2490, 1600}},
                {234, 295, 3, {3119, 1856}, {3162, 1856}},
                {232, 296, 1, {2863, 1056}, {2906, 1056}},
            };
            constexpr int32 Stair[] = {226, 228, 229, 171, 144, 141, 166};
            constexpr int32 OpenZ[] = {40, 0, -40, 67, 67, 67, 67};
            const auto* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            FVector2D Lift = FVector2D::ZeroVector;
            if (!Travel || Travel->Quest.Active != 1 || Travel->Quest.ResumeMaps.FindRef(1) != 60
                || !Level.GetSectorSurfaceHeights(225, Lift))
            {
                return Finish(&Player, TEXT("the L60 review needs the active Knights of Andrew quest"));
            }
            const double Now = Current.GetTimeSeconds();
            FName Relic(TEXT("L60 Necklace of Andrew"));
            bool bSaved = false;
            switch (NextPoint)
            {
            case 0:
            {
                FWadLevelCheckpoint Snapshot;
                const auto* Controller = Player.GetController<APlayerController>();
                const auto* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
                if (!Hud || !Hud->HasClassicHud() || !Player.HasRestoredWadCheckpoint()
                    || Lift.X != 64.0 || !Level.WriteCheckpoint(Snapshot, true)
                    || Snapshot.ScriptObjectAngles.Num() != 5 || !Snapshot.RemainingPickups.Contains(Relic)
                    || !PlaceReviewPlayer(Level, Player, {2658, 366}))
                {
                    return Finish(&Player, TEXT("L60 did not load the original setup and newly imported necklace"));
                }
                ++NextPoint;
                break;
            }
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {2666, 411, -12});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 2:
                if (Now - StageTime >= 3.10)
                {
                    if (!FMath::IsNearlyEqual(Lift.X, -50.0, 0.1))
                    {
                        return Finish(&Player, TEXT("E on L60 line 1130 did not lower the authored lift"));
                    }
                    ++NextPoint;
                }
                break;
            case 3:
                if (Now - StageTime >= 9.10)
                {
                    if (!FMath::IsNearlyEqual(Lift.X, 64.0, 0.1) || !PlaceReviewPlayer(Level, Player, {2659, 230}))
                    {
                        return Finish(&Player, TEXT("L60 lift did not finish its nine-second cycle"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L60's authored E switch lowers floor 225 to Z-50 and returns it to Z64."));
                    ++NextPoint;
                }
                break;
            case 4:
                if (FVector2D(Player.GetActorLocation()).Equals({1408, -1086}, 6.0))
                {
                    Stop(Player);
                    Player.ReceiveEncounterHit(FMath::Clamp(Player.GetHealth() - 1, 0, 100));
                    const FVector2D Approach = Plates[0].Approach;
                    if (!PlaceReviewPlayer(Level, Player, Approach))
                    {
                        return Finish(&Player, TEXT("the original statue approach was missing"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L60 sector 135 teleports the player to (1408, -1086)."));
                    ++NextPoint;
                }
                else if (WalkTo(Player, {2659, 170}))
                {
                    return Finish(&Player, TEXT("the L60 teleport did not run on entering its sector"));
                }
                break;
            case 5:
            {
                if (Player.GetHealth() != Player.GetMaxHealth()
                    || !Player.GetInteractionMessage().Contains(TEXT("The enchanted waters have healed you.")))
                {
                    return Finish(&Player, TEXT("the lower pools did not run the original healing activity"));
                }
                const auto& Plate = Plates[AndrewPlateIndex];
                Aim(Player, FVector(Plate.Inside, Player.GetPawnViewLocation().Z));
                if (CaptureTownFrame(TEXT("statue-plate"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the statue puzzle"));
                    }
                    ++NextPoint;
                }
                break;
            }
            case 6:
            {
                const FPlate& Plate = Plates[AndrewPlateIndex];
                if (WalkTo(Player, Plate.Inside))
                {
                    if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != Plate.Sector)
                    {
                        return Finish(&Player, TEXT("the walk did not enter its authored statue plate"));
                    }
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            }
            case 7:
                if (Now - StageTime >= 1.05)
                {
                    constexpr uint16 Initial[] = {32768, 0, 32768, 16384, 49152};
                    const FPlate& Plate = Plates[AndrewPlateIndex];
                    FWadLevelCheckpoint Snapshot;
                    ++AndrewPlateTurns;
                    const uint16 Expected = uint16(Initial[AndrewPlateIndex] + 16384 * AndrewPlateTurns);
                    if (!Level.WriteCheckpoint(Snapshot, true) || !Snapshot.ScriptObjectAngles.Contains(Plate.Object)
                        || Snapshot.ScriptObjectAngles.FindRef(Plate.Object) != Expected
                        || Snapshot.Variables[10 + AndrewPlateIndex] != Expected / 16384)
                    {
                        return Finish(&Player, TEXT("the L60 plate did not rotate its original statue by a quarter turn"));
                    }
                    if (AndrewPlateTurns == Plate.Turns)
                    {
                        ++AndrewPlateIndex;
                        AndrewPlateTurns = 0;
                    }
                    if (AndrewPlateIndex == UE_ARRAY_COUNT(Plates))
                    {
                        if (Snapshot.Variables[16] != 10)
                        {
                            return Finish(&Player, TEXT("the original statue combination did not solve the puzzle"));
                        }
                        ++NextPoint;
                    }
                    else
                    {
                        if (Snapshot.Variables[16] == 10 || !PlaceReviewPlayer(Level, Player, Plates[AndrewPlateIndex].Approach))
                        {
                            return Finish(&Player, TEXT("an incorrect statue combination opened a crossing"));
                        }
                        --NextPoint;
                    }
                }
                break;
            case 8:
                if (Now - StageTime >= 3.2)
                {
                    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Stair); ++Index)
                    {
                        FVector2D Heights;
                        if (!Level.GetSectorSurfaceHeights(Stair[Index], Heights) || Heights.X != OpenZ[Index])
                        {
                            return Finish(&Player, TEXT("the solved puzzle did not raise each crossing and stair to source height"));
                        }
                    }
                    if (!PlaceReviewPlayer(Level, Player, {2420, 1846}))
                    {
                        return Finish(&Player, TEXT("could not reach the raised crossing approach"));
                    }
                    ++NextPoint;
                }
                break;
            case 9:
                if (WalkTo(Player, {2543, 1846}))
                {
                    Aim(Player, {2330, 1846, Player.GetPawnViewLocation().Z - 24});
                    ++NextPoint;
                }
                break;
            case 10:
                if (CaptureTownFrame(TEXT("raised-crossing"), bSaved))
                {
                    if (!bSaved || !Player.SaveWadCheckpoint(AndrewPuzzleSlot))
                    {
                        return Finish(&Player, TEXT("could not save the solved statue checkpoint"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified all five L60 plates through movement and crossed the raised floor into a saved checkpoint."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickAndrewTown(ANativeWadPlayer& Player)
        {
            const bool bSuccess = L25Review == EL25Review::L60Return;
            const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            const UNativeWadTownData* Content = NativeWadTown::FindContent(*Player.GetWorld());
            if (!Travel || !Content || !Content->Quests.IsValidIndex(1))
            {
                return Finish(&Player, TEXT("Town could not find The Knights of Andrew"));
            }
            if (Travel->ArrivingFromMap != 0)
            {
                return true;
            }
            const FWadQuestProgress& Quest = Travel->Quest;
            const FWadTownQuest& Andrew = Content->Quests[1];
            const FWadLevelCheckpoint* L60 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L60"));
            if (Quest.Active != INDEX_NONE || Quest.ResultQuest != 1 || Quest.bSucceeded != bSuccess || !Quest.bShowResult
                || Quest.Gold != AndrewStartingGold + (bSuccess ? 20 : 0) || Quest.ResumeMaps.Contains(1)
                || Player.GetItemCount(NativeWadRules::AndrewNecklace) != 0 || !Player.HasCompletedWadLevel(TEXT("L60"))
                || !L60 || L60->SectorHeights.FindRef(247).X != 290.0 || !L60->CreatureGenerators.Contains(3)
                || L60->RemainingPickups.Contains(TEXT("L60 Necklace of Andrew")) == bSuccess
                || !Andrew.Success.Contains(TEXT("With necklace in hand"))
                || !Andrew.Failure.Contains(TEXT("You wonder if there ever was a necklace"))
                || !Andrew.TurnInMessage.Contains(TEXT("Lady Marg takes the necklace."))
                || Andrew.GoldMessage != TEXT("Lady Marg gives you 20 gold!"))
            {
                return Finish(&Player, TEXT("Town did not show the imported dialogue or apply the authored quest outcome"));
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            if (!bArrivalCaptured)
            {
                bArrivalCaptured = true;
                StageTime = Now;
                NextPoint = 0;
            }
            if (Now - StageTime < 0.5)
            {
                return true;
            }
            if (NextPoint == 1)
            {
                // Move from the completion notice to Lady Marg's turn-in page.
                Press(Player, EKeys::Enter);
                ++NextPoint;
                return true;
            }
            bool bSaved = false;
            const TCHAR* Image = NextPoint == 2 ? TEXT("town-turn-in") : NextPoint == 3 ? TEXT("town-reward")
                : bSuccess ? TEXT("town-success") : TEXT("town-failure");
            if (CaptureTownFrame(Image, bSaved))
            {
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the Knights of Andrew quest outcome"));
                }
                if (bSuccess && NextPoint < 3)
                {
                    Press(Player, EKeys::Enter);
                    ++NextPoint;
                    return true;
                }
                if (bSuccess && !Player.SaveWadCheckpoint(AndrewTownSlot))
                {
                    return Finish(&Player, TEXT("could not save the campaign after The Knights of Andrew"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified The Knights of Andrew town result. Success=%d, gold reward=%d."),
                    bSuccess, Quest.Gold - AndrewStartingGold);
                return Finish(&Player);
            }
            return true;
        }

        bool TickL60Relic(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const bool bResume = L25Review == EL25Review::L60Return;
            const bool bEmpty = L25Review == EL25Review::L60Empty;
            const auto* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            FVector2D Gate = FVector2D::ZeroVector;
            if (!Travel || Travel->Quest.Active != 1 || !Level.GetSectorSurfaceHeights(247, Gate))
            {
                return Finish(&Player, TEXT("the L60 exit review needs its saved active quest"));
            }
            bool bSaved = false;
            const FVector2D Walkway[] = {{4001, 3561}, {3900, 3561}, {3660, 3561}, {3550, 3561},
                {3441, 3561}, {3441, 3625}, {3427, 3664}};
            switch (NextPoint)
            {
            case 0:
            {
                FWadLevelCheckpoint Snapshot;
                if (!Player.HasRestoredWadCheckpoint() || !Level.WriteCheckpoint(Snapshot, true)
                    || Snapshot.Variables[16] != 10 || Snapshot.ScriptObjectAngles.Num() != 5
                    || Snapshot.SectorHeights.FindRef(141).X != 67.0
                    || Player.GetItemCount(NativeWadRules::AndrewNecklace) != int32(bResume))
                {
                    return Finish(&Player, TEXT("L60 did not restore the solved statues and expected inventory"));
                }
                AndrewStartingGold = Travel->Quest.Gold;
                if (bResume)
                {
                    if (Gate.X != 290.0 || !Snapshot.CreatureGenerators.Contains(3)
                        || Level.FindWadSector(FVector2D(Player.GetActorLocation())) != 245)
                    {
                        return Finish(&Player, TEXT("L60 did not restore the opened exit and necklace checkpoint"));
                    }
                    NextPoint = 5;
                    break;
                }
                if (Gate.X != 416.0 || !PlaceReviewPlayer(Level, Player, {3975, 3518}))
                {
                    return Finish(&Player, TEXT("could not reach the original necklace pedestal approach"));
                }
                ++NextPoint;
                break;
            }
            case 1:
                if (WalkTo(Player, {4034, 3535}))
                {
                    if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != 204)
                    {
                        return Finish(&Player, TEXT("the player did not step onto the original necklace pedestal"));
                    }
                    Aim(Player, {4020, 3516, 330});
                    if (!bEmpty)
                    {
                        Press(Player, EKeys::E);
                    }
                    ++NextPoint;
                }
                break;
            case 2:
                if (Player.GetItemCount(NativeWadRules::AndrewNecklace) != (bEmpty ? 0 : 1))
                {
                    return Finish(&Player, TEXT("the original Necklace of Andrew pickup did not respond to E"));
                }
                if (Gate.X == 290.0 && CaptureTownFrame(TEXT("necklace-pedestal"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the necklace pedestal and opened gate"));
                    }
                    ++NextPoint;
                }
                break;
            case 3:
                if (WalkTo(Player, Walkway[AndrewPlateIndex]) && ++AndrewPlateIndex == UE_ARRAY_COUNT(Walkway))
                {
                    if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != 245)
                    {
                        return Finish(&Player, TEXT("walking through the original exit gate did not reach the corridor"));
                    }
                    ++NextPoint;
                }
                break;
            case 4:
                if (bEmpty)
                {
                    ++NextPoint;
                }
                else if (Player.GetCharacterMovement()->IsMovingOnGround() && Player.SaveWadCheckpoint(AndrewRelicSlot))
                {
                    UE_LOG(LogTemp, Display, TEXT("Saved the Necklace of Andrew after crossing the authored L60 exit gate."));
                    return Finish(&Player);
                }
                break;
            case 5:
                if (WalkTo(Player, {3383, 3708}))
                {
                    ++NextPoint;
                }
                break;
            case 6:
                if (WalkTo(Player, {3328, 3714}))
                {
                    return Finish(&Player, TEXT("the authored L60 teleporter did not return to Town Hall"));
                }
                break;
            }
            return true;
        }

        bool TickElmoreLaunch(ANativeWadPlayer& Player, const UNativeWadTravelState& Travel, bool bTown)
        {
            if (bTown)
            {
                if (!Player.HasRestoredWadCheckpoint() || !Player.HasCompletedWadLevel(TEXT("L60")))
                {
                    return Finish(&Player, TEXT("Elmore's Retreat needs the completed Andrew checkpoint"));
                }
                if (Travel.Quest.bShowResult)
                {
                    Press(Player, EKeys::Enter);
                    return true;
                }
                if (NextPoint == 0 && Travel.Quest.Selected == 1)
                {
                    RecordTownCharacter(Player);
                    Press(Player, EKeys::Right);
                    ++NextPoint;
                    return true;
                }
                if (Travel.Quest.Selected != 2)
                {
                    return Finish(&Player, TEXT("the original quest selector did not reach Elmore's Retreat"));
                }
                if (NextPoint == 1)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::Q);
                    ++NextPoint;
                }
                else if (NextPoint == 2)
                {
                    bool bSaved = false;
                    if (CaptureTownFrame(TEXT("briefing"), bSaved))
                    {
                        if (!bSaved)
                        {
                            return Finish(&Player, TEXT("could not capture the Elmore briefing"));
                        }
                        Press(Player, EKeys::Enter);
                        ++NextPoint;
                    }
                }
                return true;
            }
            if (!Player.GetWorld()->GetMapName().EndsWith(TEXT("L64")) || Travel.Quest.Active != 2
                || !FVector2D(Player.GetActorLocation()).Equals({1984, 2704}, 3.0)
                || !MatchesTownCharacter(Player) || !Travel.JournalPages.Contains(423) || !Travel.JournalPages.Contains(424))
            {
                return Finish(&Player, TEXT("Elmore's Retreat did not reach L64's source spawn with both journal pages"));
            }
            APlayerController* Controller = Player.GetController<APlayerController>();
            ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
            if (!Hud || !Hud->HasClassicHud())
            {
                return Finish(&Player, TEXT("the native Elmore arrival lost the classic HUD"));
            }
            const auto ClickFormButton = [&](FVector2D Point)
            {
                int32 Width, Height;
                Controller->GetViewportSize(Width, Height);
                const double Scale = FMath::Min(Width / 320.0, Height / 200.0);
                Hud->ClickHUD((FVector2D(Width, Height) - FVector2D(320, 200) * Scale) * 0.5 + Point * Scale);
            };
            bool bSaved = false;
            if (NextPoint == 3 && CaptureTownFrame(TEXT("l64-arrival"), bSaved))
            {
                if (!bSaved || !Player.SaveWadCheckpoint(ElmoreArrivalSlot))
                {
                    return Finish(&Player, TEXT("could not save the original L64 arrival"));
                }
                ClickFormButton({30, 189});
                if (!Player.IsInventoryOpen())
                {
                    return Finish(&Player, TEXT("the classic journal button did not open the authored note page"));
                }
                ++NextPoint;
            }
            if (NextPoint == 4 && CaptureTownFrame(TEXT("journal"), bSaved))
            {
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the original journal page"));
                }
                ClickFormButton({300, 132});
                ++NextPoint;
                return true;
            }
            if (NextPoint == 5 && CaptureTownFrame(TEXT("journal-page-two"), bSaved))
            {
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the second original journal page"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified Town Hall launches Elmore's Retreat at L64's original spawn with journal pages 423 and 424."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickElmoreL64(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const double Now = Player.GetWorld()->GetTimeSeconds();
            FVector2D Lift;
            const bool bResume = L25Review == EL25Review::ElmoreL64Reload;
            if (!Level.GetSectorSurfaceHeights(278, Lift))
            {
                return Finish(&Player, TEXT("L64 is missing the authored lift"));
            }
            if (bResume)
            {
                FWadLevelCheckpoint Snapshot;
                if (!Player.HasRestoredWadCheckpoint() || !Level.WriteCheckpoint(Snapshot, true))
                {
                    return Finish(&Player, TEXT("L64 did not restore the running lift from disk"));
                }
                if (NextPoint == 0 && Lift.X == 120.0 && Snapshot.ActivityWallTextures.FindRef(2685) == TEXT("SW2BRN2")
                    && Snapshot.ActivityWallTextures.FindRef(2687) == TEXT("SW2BRN2"))
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified the L64 lift completed after restart and reset both authored switch faces."));
                    if (!PlaceReviewPlayer(Level, Player, {2980, -1280}))
                    {
                        return Finish(&Player, TEXT("could not reach L64's flood controls"));
                    }
                    StageTime = Now;
                    ++NextPoint;
                }
                else if (NextPoint == 1 && Now - StageTime > 1.10)
                {
                    if (Snapshot.SectorHeights.FindRef(351).X != 96 || Snapshot.SectorHeights.FindRef(125).X != 200
                        || !PlaceReviewPlayer(Level, Player, {2990, -1380}))
                    {
                        return Finish(&Player, TEXT("L64's first flood activity did not move the authored floors"));
                    }
                    StageTime = Now;
                    ++NextPoint;
                }
                else if (NextPoint == 2 && Now - StageTime > 1.10)
                {
                    if (Snapshot.SectorHeights.FindRef(351).X != 0 || Snapshot.SectorHeights.FindRef(128).X != 16
                        || !PlaceReviewPlayer(Level, Player, {-762, 108}))
                    {
                        return Finish(&Player, TEXT("L64's second flood activity did not restore the authored floor heights"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified both L64 flood activities at their authored floor heights."));
                    ++NextPoint;
                }
                else if (NextPoint == 3 && WalkTo(Player, {-907, 108}))
                {
                    return Finish(&Player, TEXT("the authored L64 exit did not travel to L65"));
                }
                return true;
            }

            switch (NextPoint)
            {
            case 0:
                if (!Player.HasRestoredWadCheckpoint() || Lift.X != 120 || !PlaceReviewPlayer(Level, Player, {-123, -1685}))
                {
                    return Finish(&Player, TEXT("L64 did not restore the authored lift initially at Z120"));
                }
                ++NextPoint;
                break;
            case 1:
                if (WalkTo(Player, {-123, -1734}))
                {
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 2:
                if (Now - StageTime > 2.05)
                {
                    if (Lift.X != 0 || !PlaceReviewPlayer(Level, Player, {-193, -1989}))
                    {
                        return Finish(&Player, TEXT("L64's original plate did not lower the lift"));
                    }
                    ++NextPoint;
                }
                break;
            case 3:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-153.5, -1987.5, 45});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 4:
                if (Lift.X > 0 && Lift.X < 120)
                {
                    FWadLevelCheckpoint Snapshot;
                    if (!Level.WriteCheckpoint(Snapshot, true)
                        || Snapshot.ActivityWallTextures.FindRef(2685) != TEXT("SW2BRN1")
                        || Snapshot.ActivityWallTextures.FindRef(2687) != TEXT("SW2BRN1"))
                    {
                        return Finish(&Player, TEXT("L64's lift switch did not change both authored faces"));
                    }
                    if (Lift.X >= 50 && Player.SaveWadCheckpoint(ElmoreSlideSlot))
                    {
                        UE_LOG(LogTemp, Display, TEXT("Verified L64's original plate and E switch and saved the lift halfway through its cycle."));
                        return Finish(&Player);
                    }
                }
                else if (Now - StageTime > 0.55)
                {
                    return Finish(&Player, TEXT("the L64 lift switch did not produce a checkpoint during motion"));
                }
                break;
            }
            return true;
        }

        bool TickElmoreL65(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const bool bResume = L25Review == EL25Review::ElmoreL65Reload;
            const FVector2D Destination(1628, 2370);
            FWadLevelCheckpoint Snapshot;
            if (!Player.HasRestoredWadCheckpoint() || !Level.WriteCheckpoint(Snapshot, true))
            {
                return Finish(&Player, TEXT("L65 did not restore the active quest checkpoint"));
            }
            if (bResume)
            {
                if (NextPoint == 0)
                {
                    if (!FVector2D(Player.GetActorLocation()).Equals(Destination, 6)
                        || Snapshot.ActivityWallTextures.FindRef(1222) != TEXT("SW2BRN1") || Snapshot.Variables[1] != 1)
                    {
                        return Finish(&Player, TEXT("L65 did not restore the teleporter position and switch state"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L65 restored the authored teleporter destination and its switch."));
                }
                return TickElmoreBillboards(Level, Player);
            }
            switch (NextPoint)
            {
            case 0:
                if (!FVector2D(Player.GetActorLocation()).Equals({-852, 2282}, 3)
                    || !PlaceReviewPlayer(Level, Player, {1221, 2360}))
                {
                    return Finish(&Player, TEXT("could not reach the first L65 switch from the checkpoint"));
                }
                ++NextPoint;
                break;
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {1221, 2400, 144});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 2:
                if (Snapshot.ActivityWallTextures.FindRef(1222) != TEXT("SW2BRN1") || Snapshot.Variables[1] != 1
                    || !PlaceReviewPlayer(Level, Player, {1120, 2310}))
                {
                    return Finish(&Player, TEXT("the first L65 switch did not enable its teleporter"));
                }
                ++NextPoint;
                break;
            case 3:
                if (FVector2D(Player.GetActorLocation()).Equals(Destination, 6))
                {
                    Stop(Player);
                    ++NextPoint;
                }
                else if (WalkTo(Player, {1120, 2373}))
                {
                    return Finish(&Player, TEXT("walking into sector 115 did not use L65's enabled teleporter"));
                }
                break;
            case 4:
                if (Player.GetCharacterMovement()->IsMovingOnGround() && Player.SaveWadCheckpoint(ElmoreTeleporterSlot))
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L65 line 847 enables sector 115's original teleporter to (1628, 2370)."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickElmoreBillboards(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            if (NextPoint == 0 || NextPoint == 2)
            {
                const FVector2D Point = NextPoint == 0 ? FVector2D(-776, 1816) : FVector2D(-650, 1930);
                if (!PlaceReviewPlayer(Level, Player, Point))
                {
                    return Finish(&Player, TEXT("could not reach the L65 scenery review view"));
                }
                Aim(Player, {-744, 1908, 67});
                ++NextPoint;
                return true;
            }
            bool bHanging = false;
            bool bFloor = false;
            const APlayerController* Controller = Player.GetController<APlayerController>();
            FVector Eye;
            FRotator View;
            Controller->GetPlayerViewPoint(Eye, View);
            for (TActorIterator<ANativeWadScenery> Prop(Player.GetWorld()); Prop; ++Prop)
            {
                const bool bBraziers = Prop->ActorHasTag(TEXT("WAD Scenery Thing 156"));
                if (!bBraziers && !Prop->ActorHasTag(TEXT("WAD Scenery Thing 209")))
                {
                    continue;
                }
                const FVector Anchor = Prop->GetActorLocation();
                const UStaticMeshComponent* Sprite = Prop->FindComponentByClass<UStaticMeshComponent>();
                const int32 Sector = Level.FindWadSector(FVector2D(Anchor));
                FVector2D Surfaces;
                if (!Sprite || !Sprite->GetStaticMesh() || !Level.GetSectorSurfaceHeights(Sector, Surfaces))
                {
                    return Finish(&Player, TEXT("could not inspect L65's barrel and animated hanging brazier"));
                }
                const FVector Facing = (Eye - Anchor).GetSafeNormal2D();
                const FVector Center = Sprite->GetComponentLocation();
                const double HalfHeight = Sprite->GetStaticMesh()->GetBounds().BoxExtent.Y
                    * FMath::Abs(Sprite->GetComponentTransform().GetScale3D().Y);
                const double Edge = Center.Z + (bBraziers ? HalfHeight : -HalfHeight);
                const double Surface = bBraziers ? Surfaces.Y : Level.GetWalkingHeight(Sector);
                if (FVector::DotProduct(Sprite->GetUpVector(), -Facing) < 0.995
                    || !FVector2D(Center).Equals(FVector2D(Anchor), 0.05)
                    || !FMath::IsNearlyEqual(Edge, Surface, 0.05)
                    || !FMath::IsNearlyEqual(Anchor.Z, Surface, 0.05)
                    || Prop->GetSourceAngle() != 0)
                {
                    return Finish(&Player, TEXT("an L65 sprite failed to face the camera upright around its authored mount"));
                }
                (bBraziers ? bHanging : bFloor) = true;
            }
            if (!bHanging || !bFloor)
            {
                return Finish(&Player, TEXT("the L65 scenery mount fixtures are missing"));
            }
            bool bSaved = false;
            if (CaptureTownFrame(NextPoint == 1 ? TEXT("l65-sprites-south") : TEXT("l65-sprites-east"), bSaved))
            {
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the L65 scenery from the second view"));
                }
                if (++NextPoint == 4)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L65's barrel and animated brazier face both views with fixed floor and ceiling mounts."));
                    return Finish(&Player);
                }
            }
            return true;
        }

        bool TickElmoreCombat(ANativeWadLevel& Level, ANativeWadPlayer& Player, bool bRanged)
        {
            const FName TargetTag(bRanged ? TEXT("WAD Creature Thing 247") : TEXT("WAD Creature Thing 404"));
            ANativeWadGuardian* Enemy = nullptr;
            int32 Count = 0;
            for (TActorIterator<ANativeWadGuardian> Candidate(Player.GetWorld()); Candidate; ++Candidate)
            {
                ++Count;
                if (Candidate->ActorHasTag(TargetTag))
                {
                    Enemy = *Candidate;
                }
            }
            if (!Enemy || Count != (bRanged ? 31 : 28) || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the authored barbarian encounter placements are incomplete"));
            }
            if (NextPoint == 0)
            {
                const FVector2D Point = bRanged ? FVector2D(2261, 1905) : FVector2D(-493, 87);
                if (Enemy->GetEncounterHealth() != 2500 || !PlaceReviewPlayer(Level, Player, Point))
                {
                    return Finish(&Player, TEXT("the barbarian fixture lost its original health or approach"));
                }
                HealthBeforeTravel = Player.GetHealth();
                Aim(Player, Enemy->GetActorLocation());
                StageTime = Player.GetWorld()->GetTimeSeconds();
                ++NextPoint;
                return true;
            }
            if (NextPoint == 1)
            {
                const int32 Damage = HealthBeforeTravel - Player.GetHealth();
                const bool bAttacked = bRanged ? Enemy->GetBoltsFired() > 0 : Enemy->GetMeleeHits() == 1;
                if (bAttacked && Damage == (bRanged ? 100 : 350))
                {
                    ++NextPoint;
                }
                else if (Damage > (bRanged ? 100 : 350) || Player.GetWorld()->GetTimeSeconds() - StageTime > 5.0)
                {
                    return Finish(&Player, TEXT("the barbarian missed its authored attack or inflicted unexpected damage"));
                }
                else
                {
                    return true;
                }
            }
            bool bSaved = false;
            if (CaptureTownFrame(bRanged ? TEXT("white-barbarian") : TEXT("bronze-barbarian"), bSaved))
            {
                UE_LOG(LogTemp, Display, TEXT("%s"), bRanged ? TEXT("Verified the L65 White Barbarian's 100-damage magic dart.")
                    : TEXT("Verified the L64 Bronze Barbarian's 350-damage melee attack."));
                return Finish(&Player, bSaved ? nullptr : TEXT("could not capture the authored encounter"));
            }
            return true;
        }

        bool TickElmoreL65Exit(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D WestDoor, Lift, EastDoor;
            if (!Level.GetSectorSurfaceHeights(154, WestDoor) || !Level.GetSectorSurfaceHeights(153, Lift)
                || !Level.GetSectorSurfaceHeights(172, EastDoor))
            {
                return Finish(&Player, TEXT("L65's authored exit lift and doors are missing"));
            }
            switch (NextPoint)
            {
            case 0:
                if (!Player.HasRestoredWadCheckpoint() || !FVector2D(Player.GetActorLocation()).Equals({1628, 2370}, 6))
                {
                    return Finish(&Player, TEXT("the L65 exit replay needs the first teleporter checkpoint"));
                }
                Press(Player, EKeys::NumPadFour);
                ++NextPoint;
                break;
            case 1:
                Press(Player, EKeys::NumPadFive);
                ++NextPoint;
                break;
            case 2:
                Press(Player, EKeys::Enter);
                ++NextPoint;
                break;
            case 3:
                if (WalkTo(Player, {1470, 2368}))
                {
                    Aim(Player, {1466, 2400, 144});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 4:
            {
                FWadLevelCheckpoint State;
                if (!Level.WriteCheckpoint(State, true) || State.ActivityWallTextures.FindRef(748) != TEXT("SW2BRN1")
                    || State.Variables[2] != 1)
                {
                    return Finish(&Player, TEXT("L65 line 533 did not enable the second teleporter"));
                }
                ++NextPoint;
                break;
            }
            case 5:
                if (FVector2D(Player.GetActorLocation()).Equals({2234, 1600}, 6))
                {
                    Stop(Player);
                    UE_LOG(LogTemp, Display, TEXT("Verified L65's second switch enables the teleporter to (2234, 1600)."));
                    if (!PlaceReviewPlayer(Level, Player, {2624, 1619}))
                    {
                        return Finish(&Player, TEXT("could not reach the L65 lift approach"));
                    }
                    ++NextPoint;
                }
                else if (WalkTo(Player, {1628, 2370}))
                {
                    return Finish(&Player, TEXT("L65's second teleporter did not send the player to its authored destination"));
                }
                break;
            case 6:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {2656, 1619, 233});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 7:
                if (WestDoor.Y - WestDoor.X >= 58 && WalkTo(Player, {2706, 1619}))
                {
                    StageTime = Player.GetWorld()->GetTimeSeconds();
                    ++NextPoint;
                }
                break;
            case 8:
                if (Player.GetWorld()->GetTimeSeconds() - StageTime > 1.05 && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    if (Lift.X != 0)
                    {
                        return Finish(&Player, TEXT("L65's alternating lift did not lower to the east door"));
                    }
                    Aim(Player, {2733, 1619, 42});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 9:
                if (EastDoor.Y - EastDoor.X >= 58)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified both authored L65 lift doors and the lowering floor in sector 153."));
                    ++NextPoint;
                }
                break;
            case 10:
                if (WalkTo(Player, {2852, 1619}))
                {
                    return Finish(&Player, TEXT("L65's original exit marker did not lead to L34"));
                }
                break;
            }
            return true;
        }

        bool TickElmoreL34(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            const double Now = Player.GetWorld()->GetTimeSeconds();
            FVector2D Trap, Exit;
            if (!Level.GetSectorSurfaceHeights(148, Trap) || !Level.GetSectorSurfaceHeights(33, Exit))
            {
                return Finish(&Player, TEXT("L34's original trap floor and exit are missing"));
            }
            switch (NextPoint)
            {
            case 0:
                if (!Player.HasRestoredWadCheckpoint() || Exit.X != 104 || Trap.X != 0)
                {
                    return Finish(&Player, TEXT("the L34 mechanism replay needs its authored starting state"));
                }
                if (LighthouseHealInputs < 6 && Player.GetMana() >= 1000 && Player.GetHealth() < Player.GetMaxHealth())
                {
                    const FKey Runes[] = {EKeys::NumPadFour, EKeys::NumPadFive, EKeys::Enter};
                    Press(Player, Runes[LighthouseHealInputs++ % UE_ARRAY_COUNT(Runes)]);
                    return true;
                }
                if (!PlaceReviewPlayer(Level, Player, {-368, -2975}))
                {
                    return Finish(&Player, TEXT("could not reach the L34 trap approach"));
                }
                ++NextPoint;
                break;
            case 1:
                if (WalkTo(Player, {-373, -3036}))
                {
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 2:
                if (Now - StageTime > 0.35)
                {
                    if (Trap.X != -200)
                    {
                        return Finish(&Player, TEXT("the authored L34 pressure plate did not drop to Z-200"));
                    }
                    ++NextPoint;
                }
                break;
            case 3:
                if (Now - StageTime > 7.2)
                {
                    if (Trap.X != 0 || !PlaceReviewPlayer(Level, Player, {611, -3088}))
                    {
                        return Finish(&Player, TEXT("L34's trap did not reset to the original floor"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L34's original pressure plate drops its floor to Z-200 and resets to Z0."));
                    ++NextPoint;
                }
                break;
            case 4:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {656, -3088, 40});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 5:
                if (Now - StageTime > 2.15)
                {
                    FWadLevelCheckpoint State;
                    if (Exit.X != 32 || !Level.WriteCheckpoint(State, true)
                        || State.ActivityWallTextures.FindRef(1221) != TEXT("SWIT1U")
                        || !PlaceReviewPlayer(Level, Player, {-480, 978}))
                    {
                        return Finish(&Player, TEXT("L34's authored E switch did not open the lighthouse exit"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L34 line 914 lowers exit floor 33 from Z104 to Z32."));
                    ++NextPoint;
                }
                break;
            case 6:
                NextPoint += WalkTo(Player, {-353, 978}) ? 1 : 0;
                break;
            case 7:
                if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) == 184 && Player.GetActorLocation().Y <= 885)
                {
                    Stop(Player);
                    UE_LOG(LogTemp, Display, TEXT("Verified player movement across the opened L34 gate into sector 184."));
                    if (!PlaceReviewPlayer(Level, Player, {-132, 852}))
                    {
                        return Finish(&Player, TEXT("could not reach the L34 exit approach after the gate crossing"));
                    }
                    ++NextPoint;
                }
                else
                {
                    WalkTo(Player, {-353, 876});
                }
                break;
            case 8:
                if (WalkTo(Player, {-29, 852}))
                {
                    return Finish(&Player, TEXT("L34's original exit marker did not lead to L151"));
                }
                break;
            }
            return true;
        }

        bool TickElmoreTravel(ANativeWadLevel& Level, ANativeWadPlayer& Player, const UNativeWadTravelState& Travel)
        {
            const FString Map = Player.GetWorld()->GetMapName();
            const bool bL34 = Map.EndsWith(TEXT("L34"));
            const bool bL151 = Map.EndsWith(TEXT("L151"));
            if (L25Review == EL25Review::ElmoreL65Exit && !bL34)
            {
                return TickElmoreL65Exit(Level, Player);
            }
            if (L25Review == EL25Review::ElmoreL34 && !bL151)
            {
                return TickElmoreL34(Level, Player);
            }
            const int32 Destination = bL34 ? 34 : bL151 ? 151 : 0;
            const bool bReload = L25Review == EL25Review::ElmoreL151Reload;
            const FWadLevelCheckpoint* L65 = Travel.VisitedLevels.Find(TEXT("/Game/Maps/L65"));
            const FWadLevelCheckpoint* L34 = Travel.VisitedLevels.Find(TEXT("/Game/Maps/L34"));
            if (!Destination || Travel.Quest.ResumeMaps.FindRef(2) != Destination
                || !L65 || L65->ActivityWallTextures.FindRef(748) != TEXT("SW2BRN1")
                || !FVector2D(Player.GetActorLocation()).Equals(bL34 ? FVector2D(912, 1596) : FVector2D(176, 3464), 5)
                || (bL151 && (!L34 || L34->SectorHeights.FindRef(33).X != 32 || L34->Encounters.Num() != 30))
                || (bReload && !Player.HasRestoredWadCheckpoint()))
            {
                return Finish(&Player, TEXT("the Elmore handoff lost the authored destination or prior quest state"));
            }
            bool bSaved = false;
            const TCHAR* Image = bL34 ? TEXT("l34-arrival") : bReload ? TEXT("l151-reloaded") : TEXT("l151-arrival");
            if (CaptureTownFrame(Image, bSaved))
            {
                if (!bSaved || (!bReload && !Player.SaveWadCheckpoint(bL34 ? ElmoreL34Slot : ElmoreL151Slot)))
                {
                    return Finish(&Player, TEXT("could not verify and save the authored Elmore destination"));
                }
                UE_LOG(LogTemp, Display, TEXT("%s"), bL34 ? TEXT("Verified L65 Thing 239 takes Elmore's Retreat to the L34 lighthouse.")
                    : bReload ? TEXT("Verified L151 reopens with the active quest and archived L65 and L34 states.")
                    : TEXT("Verified L34 Thing 180 takes the active quest to L151 and archives the lighthouse."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickElmoreMountainExit(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D CaveDoor;
            if (!Player.HasRestoredWadCheckpoint() || !Level.GetSectorSurfaceHeights(48, CaveDoor))
            {
                return Finish(&Player, TEXT("the L151 cave needs its original checkpoint and exit door"));
            }
            switch (NextPoint)
            {
            case 0:
                if (!PlaceReviewPlayer(Level, Player, {2624, 3051}))
                {
                    return Finish(&Player, TEXT("could not reach the original L151 key"));
                }
                ++NextPoint;
                break;
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {2586, 3051, -52});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 2:
                if (Player.GetItemCount(NativeWadRules::ElmoreKey5) != 1 || !PlaceReviewPlayer(Level, Player, {2624, 4204}))
                {
                    return Finish(&Player, TEXT("could not collect Key 5 from the old checkpoint or reach the cave"));
                }
                ++NextPoint;
                break;
            case 3:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {2624, 4256, 9});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 4:
                if (CaveDoor.Y - CaveDoor.X >= 58 && WalkTo(Player, {2624, 4336}))
                {
                    return Finish(&Player, TEXT("the authored L151 cave exit did not lead to L152"));
                }
                break;
            }
            return true;
        }

        bool TickElmoreReservoir(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D Gate;
            if (!Player.HasRestoredWadCheckpoint() || !Level.GetSectorSurfaceHeights(27, Gate))
            {
                return Finish(&Player, TEXT("the L152 reservoir needs its saved arrival and movable floor"));
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            switch (NextPoint)
            {
            case 0:
                if (Gate.X != 24 || !PlaceReviewPlayer(Level, Player, {-422, -130}))
                {
                    return Finish(&Player, TEXT("could not reach L152's original key approach"));
                }
                ++NextPoint;
                break;
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-388, -130, 140});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 2:
                if (Player.GetItemCount(NativeWadRules::ElmoreKey6) != 1 || !PlaceReviewPlayer(Level, Player, {-568, -152}))
                {
                    return Finish(&Player, TEXT("could not collect Key 6 or reach the L152 hidden switch"));
                }
                ++NextPoint;
                break;
            case 3:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-531.5, -134, 77});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 4:
                if (Now - StageTime > 0.65)
                {
                    FWadLevelCheckpoint State;
                    if (Gate.X != -476 || !Level.WriteCheckpoint(State, true) || State.ActivityWallTextures.FindRef(478) != TEXT("SWIT1U")
                        || !PlaceReviewPlayer(Level, Player, {-350, -574}))
                    {
                        return Finish(&Player, TEXT("L152 line 305 did not lower the reservoir's floor"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L152's hidden switch lowers floor 27 from Z24 to Z-476."));
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 5:
                if (Now - StageTime > 7.2)
                {
                    FWadLevelCheckpoint State;
                    if (Level.GetScriptMissileCount(4012) != 5 || !Level.WriteCheckpoint(State, true) || !State.ScriptLockedDoors.Contains(70)
                        || !PlaceReviewPlayer(Level, Player, {-25, -94}))
                    {
                        return Finish(&Player, TEXT("L152's delayed five-shot volley did not lock the authored door"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L152 sector 69 fires five lock missiles and locks door 70."));
                    ++NextPoint;
                }
                break;
            case 6:
                if (WalkTo(Player, {-16, -250}))
                {
                    return Finish(&Player, TEXT("the original L152 exit marker did not lead to L153"));
                }
                break;
            }
            return true;
        }

        bool TickElmoreVault(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D West, Vault;
            if (!Player.HasRestoredWadCheckpoint() || !Level.GetSectorSurfaceHeights(218, West)
                || !Level.GetSectorSurfaceHeights(202, Vault))
            {
                return Finish(&Player, TEXT("the L153 review needs the original combination gates"));
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            FWadLevelCheckpoint State;
            bool bSaved = false;
            switch (NextPoint)
            {
            case 0:
                if (West.Y != 200 || Vault.X != 0 || !PlaceReviewPlayer(Level, Player, {1823, -793}))
                {
                    return Finish(&Player, TEXT("the original L153 combination puzzle starts incorrectly"));
                }
                ++NextPoint;
                break;
            case 1:
                if (WalkTo(Player, {1823, -853}))
                {
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 2:
                if (Now - StageTime > 2.15)
                {
                    if (West.Y != -16 || !PlaceReviewPlayer(Level, Player, {1840, -2035}))
                    {
                        return Finish(&Player, TEXT("the L153 plate did not close the western gate"));
                    }
                    ++NextPoint;
                }
                break;
            case 3:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {1840, -2064, 0});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 4:
                if (Now - StageTime > 2.15)
                {
                    if (West.Y != 200 || !Level.WriteCheckpoint(State, true) || State.Variables[16] != 0
                        || State.ActivityWallTextures.FindRef(1759) != TEXT("SWIT1D") || !PlaceReviewPlayer(Level, Player, {4394, -16}))
                    {
                        return Finish(&Player, TEXT("L153's original west bank combination did not reopen the gate"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L153's west switch bank reopens ceiling 218 after its plate closes it."));
                    ++NextPoint;
                }
                break;
            case 5:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {4432, -16, -110});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 6:
                if (!Level.WriteCheckpoint(State, true) || State.Variables[19] != 1 || Vault.X != 0
                    || State.ActivityWallTextures.FindRef(1487) != TEXT("SWIT1U"))
                {
                    return Finish(&Player, TEXT("an incorrect orb bank combination changed the vault floor"));
                }
                Press(Player, EKeys::E);
                ++NextPoint;
                break;
            case 7:
                if (!Level.WriteCheckpoint(State, true) || State.Variables[19] != 0
                    || State.ActivityWallTextures.FindRef(1487) != TEXT("SWIT1D"))
                {
                    return Finish(&Player, TEXT("the L153 orb switch did not toggle back to its original texture"));
                }
                // The incorrect combination locks the vault for the authored two-second reset.
                if (State.Variables[23] == 0)
                {
                    if (!PlaceReviewPlayer(Level, Player, {4394, -240}))
                    {
                        return Finish(&Player, TEXT("could not reach the L153 vault switch approach"));
                    }
                    ++NextPoint;
                }
                break;
            case 8:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {4432, -240, -112});
                    Press(Player, EKeys::E);
                    StageTime = Now;
                    ++NextPoint;
                }
                break;
            case 9:
                if (Now - StageTime > 2.15)
                {
                    if (Vault.X != -148 || !Level.WriteCheckpoint(State, true) || State.Variables[21] != 1
                        || State.ActivityWallTextures.FindRef(1516) != TEXT("SWIT1U") || !PlaceReviewPlayer(Level, Player, {4394, -160}))
                    {
                        return Finish(&Player, TEXT("the original L153 orb combination did not open the vault"));
                    }
                    Aim(Player, {4510, -160, -108});
                    ++NextPoint;
                }
                break;
            case 10:
                if (CaptureTownFrame(TEXT("vault-open"), bSaved))
                {
                    if (!bSaved || !Player.SaveWadCheckpoint(ElmoreVaultSlot))
                    {
                        return Finish(&Player, TEXT("could not save the solved L153 vault"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L153 rejects the wrong switch combination and opens the vault with the authored combination."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickElmoreOrb(ANativeWadLevel& Level, ANativeWadPlayer& Player, const UNativeWadTravelState& Travel)
        {
            const bool bCollect = L25Review == EL25Review::ElmoreOrb;
            const bool bReturn = L25Review == EL25Review::ElmoreOrbReturn;
            const int32 Orb = NativeWadRules::OrbOfDefense;
            switch (NextPoint)
            {
            case 0:
            {
                FWadLevelCheckpoint State;
                if (!Player.HasRestoredWadCheckpoint() || !Level.WriteCheckpoint(State, true)
                    || State.SectorHeights.FindRef(202).X != -148 || State.Variables[21] != 1
                    || Player.GetItemCount(Orb) != int32(bReturn))
                {
                    return Finish(&Player, TEXT("L153 did not restore the solved bank and expected orb inventory"));
                }
                if (!bCollect)
                {
                    ExperienceBeforeTravel = Player.GetExperience();
                    AndrewStartingGold = Travel.Quest.Gold;
                    if (!PlaceReviewPlayer(Level, Player, {832, -204}))
                    {
                        return Finish(&Player, TEXT("could not reach L153's original return teleporter approach"));
                    }
                }
                ++NextPoint;
                break;
            }
            case 1:
                if (!bCollect)
                {
                    if (WalkTo(Player, {832, -282}))
                    {
                        return Finish(&Player, TEXT("the original L153 exit did not return to Town Hall"));
                    }
                    break;
                }
                NextPoint += WalkTo(Player, {4485, -160}) ? 1 : 0;
                break;
            case 2:
                NextPoint += WalkTo(Player, {4612, -164}) ? 1 : 0;
                break;
            case 3:
                if (WalkTo(Player, {4612, -291}))
                {
                    Aim(Player, {4612, -330, -136});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 4:
            {
                bool bSaved = false;
                if (Player.GetItemCount(Orb) != 1)
                {
                    return Finish(&Player, TEXT("E did not collect the original Orb of Defense"));
                }
                if (CaptureTownFrame(TEXT("orb-picked-up"), bSaved))
                {
                    if (!bSaved || !Player.SaveWadCheckpoint(ElmoreOrbSlot))
                    {
                        return Finish(&Player, TEXT("could not save the Orb of Defense checkpoint"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified entry through the solved L153 gate and collection of the Orb of Defense."));
                    return Finish(&Player);
                }
                break;
            }
            }
            return true;
        }

        bool TickElmoreOrbTown(ANativeWadPlayer& Player, const UNativeWadTravelState& Travel)
        {
            const bool bSuccess = L25Review == EL25Review::ElmoreOrbReturn;
            const auto* Content = NativeWadTown::FindContent(*Player.GetWorld());
            if (Travel.ArrivingFromMap != 0)
            {
                return true;
            }
            const FWadQuestProgress& Quest = Travel.Quest;
            const FWadLevelCheckpoint* L153 = Travel.VisitedLevels.Find(TEXT("/Game/Maps/L153"));
            if (!Content || !Content->Quests.IsValidIndex(2) || Quest.Active != INDEX_NONE || Quest.ResultQuest != 2
                || Quest.bSucceeded != bSuccess || !Quest.bShowResult || Quest.ResumeMaps.Contains(2)
                || Quest.Gold != AndrewStartingGold || Player.GetExperience() != ExperienceBeforeTravel + (bSuccess ? 150000 : 0)
                || Player.GetItemCount(NativeWadRules::OrbOfDefense) != 0 || !Player.HasCompletedWadLevel(TEXT("L153"))
                || !L153 || L153->RemainingPickups.Contains(TEXT("L153 Orb of Defense")) == bSuccess)
            {
                return Finish(&Player, TEXT("Town Hall did not apply the original Orb of Defense quest outcome"));
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            if (!bArrivalCaptured)
            {
                StageTime = Now;
                bArrivalCaptured = true;
            }
            bool bSaved = false;
            if (Now - StageTime > 0.5 && CaptureTownFrame(bSuccess ? TEXT("town-success") : TEXT("town-empty"), bSaved))
            {
                if (bSuccess && !Player.SaveWadCheckpoint(ElmoreTownSlot))
                {
                    return Finish(&Player, TEXT("could not save the Elmore Town Hall result"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified Elmore's Retreat Town Hall result. Success=%d, experience reward=%d."),
                    bSuccess, Player.GetExperience() - ExperienceBeforeTravel);
                return Finish(&Player, bSaved ? nullptr : TEXT("could not capture the original Elmore result"));
            }
            return true;
        }

        bool TickUndeadCastle(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D Lift;
            Level.GetSectorSurfaceHeights(263, Lift);
            if (NextPoint == 0 && PlaceReviewPlayer(Level, Player, {1648, 2460}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 1 && WalkTo(Player, {1648, 2392}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 2 && Lift.X == 320.0)
            {
                ++NextPoint;
            }
            else if (NextPoint == 3 && Lift.X == 0.0 && PlaceReviewPlayer(Level, Player, {861, -250}))
            {
                UE_LOG(LogTemp, Display, TEXT("Verified the L231 pressure plate raises floor 263 to 320 and returns it to zero."));
                ++NextPoint;
            }
            else if (NextPoint == 4)
            {
                WalkTo(Player, {861, -319});
            }
            return true;
        }

        bool TickUndeadTraps(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D Crusher, Lift;
            Level.GetSectorSurfaceHeights(105, Crusher);
            Level.GetSectorSurfaceHeights(48, Lift);
            if (NextPoint == 0 && PlaceReviewPlayer(Level, Player, {2034, 1687}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 1 && WalkTo(Player, {2084, 1659}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 2 && Crusher.Y <= 120)
            {
                ++NextPoint;
            }
            else if (NextPoint == 3 && Crusher.Y == 224 && PlaceReviewPlayer(Level, Player, {1, 1023}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 4 && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                Aim(Player, {29, 1023, 44});
                Press(Player, EKeys::E);
                ++NextPoint;
            }
            else if (NextPoint == 5 && Lift.X == 16 && PlaceReviewPlayer(Level, Player, {-887, 1009}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 6 && WalkTo(Player, {-950, 1009}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 7 && Lift.X == 96 && PlaceReviewPlayer(Level, Player, {1528, 511}))
            {
                UE_LOG(LogTemp, Display, TEXT("Verified L75's crusher cycles and the original switch lowers the first exit lift before it rises again."));
                ++NextPoint;
            }
            else if (NextPoint == 8 && WalkTo(Player, {1528, 465}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 9)
            {
                FWadLevelCheckpoint Snapshot;
                if (Level.WriteCheckpoint(Snapshot, true) && Snapshot.CreatureGenerators.FindRef(2).Released == 1
                    && Snapshot.CreatureGenerators.FindRef(3).Released == 1 && PlaceReviewPlayer(Level, Player, {-975, -210}))
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L75's trap releases both authored Evil Knights."));
                    ++NextPoint;
                }
            }
            else if (NextPoint == 10)
            {
                WalkTo(Player, {-1035, -210});
            }
            return true;
        }

        bool TickUndeadKeep(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D Platform, DoorHeights;
            Level.GetSectorSurfaceHeights(120, Platform);
            Level.GetSectorSurfaceHeights(200, DoorHeights);
            switch (NextPoint)
            {
            case 0:
                CapturedItem = Player.GetItemCount(NativeWadRules::ElmoreKey6);
                if (PlaceReviewPlayer(Level, Player, {-499, -48})) { ++NextPoint; }
                break;
            case 1:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-471, -48, 164});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 2:
                if (Platform.X == 100 && PlaceReviewPlayer(Level, Player, {-2417, 1696})) { ++NextPoint; }
                break;
            case 3:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-2445, 1696, 44});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 4:
                if (Platform.X == 0 && PlaceReviewPlayer(Level, Player, {-983, 1793})) { ++NextPoint; }
                break;
            case 5:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-956, 1771, 12});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 6:
                if (Player.GetItemCount(NativeWadRules::ElmoreKey6) == CapturedItem + 1
                    && PlaceReviewPlayer(Level, Player, {-969, 2905})) { ++NextPoint; }
                break;
            case 7:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-969, 2939, 128});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 8:
                if (DoorHeights.Y - DoorHeights.X >= 56 && WalkTo(Player, {-969, 2980}))
                {
                    if (!PlaceReviewPlayer(Level, Player, {-804, 3175}))
                    {
                        return Finish(&Player, TEXT("could not reach the L10 exit approach"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified both L10 switches, Key 6 riding its platform, and crossing the native key door."));
                    ++NextPoint;
                }
                break;
            case 9:
                WalkTo(Player, {-740, 3175});
                break;
            }
            return true;
        }

        bool FightCampaignEncounter(ANativeWadPlayer& Player, ANativeWadGuardian& Boss)
        {
            const FVector Target = Boss.GetActorLocation() + FVector::UpVector
                * Boss.GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.35;
            if (!bFightingForeground && CampaignRoute && (CampaignRoute->Map == 72 || CampaignRoute->Map == 73 || CampaignRoute->Map == 61))
            {
                ANativeWadGuardian* Attacker = nullptr;
                double Closest = FMath::Min(FMath::Square(55.0), FVector::DistSquared2D(Player.GetActorLocation(), Target) - 225.0);
                for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                {
                    const double Range = FVector::DistSquared2D(Player.GetActorLocation(), Enemy->GetActorLocation());
                    if (*Enemy != &Boss && Enemy->GetSourceType() == (CampaignRoute->Map == 61 ? 1023 : 1011) && !Enemy->IsHidden() && !Enemy->IsDefeated()
                        && Range < Closest && Enemy->CanSeePlayer(Player)) { Attacker = *Enemy; Closest = Range; }
                }
                if (Attacker)
                {
                    bFightingForeground = true;
                    FightCampaignEncounter(Player, *Attacker);
                    bFightingForeground = false;
                    return false;
                }
            }
            if (!bFightingForeground && !Boss.IsDefeated())
            {
                const FVector Start = Player.GetPawnViewLocation();
                FHitResult Hit;
                const FCollisionQueryParams Query(SCENE_QUERY_STAT(CampaignForegroundEnemy), false, &Player);
                Player.GetWorld()->LineTraceSingleByChannel(Hit, Start,
                    Start + (Target - Start).GetSafeNormal() * 100.0, ECC_Visibility, Query);
                if (auto* Foreground = Cast<ANativeWadGuardian>(Hit.GetActor()); Foreground && Foreground != &Boss && !Foreground->IsDefeated())
                {
                    bFightingForeground = true;
                    FightCampaignEncounter(Player, *Foreground);
                    bFightingForeground = false;
                    return false;
                }
            }
            const FVector View = Player.GetPawnViewLocation();
            FHitResult MeleeSight;
            const FCollisionQueryParams SightQuery(SCENE_QUERY_STAT(CampaignMeleeSight), false, &Player);
            Player.GetWorld()->LineTraceSingleByChannel(MeleeSight, View,
                View + (Target - View).GetSafeNormal() * 110.0, ECC_Visibility, SightQuery);
            int32 Strafe = 0;
            if (Cast<ANativeWadScenery>(MeleeSight.GetActor()) && CampaignRoute && (CampaignRoute->Map == 73 || CampaignRoute->Map == 61))
            {
                const FVector Toward = (Target - Player.GetActorLocation()).GetSafeNormal2D();
                const FVector Right(-Toward.Y, Toward.X, 0);
                const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
                const UCapsuleComponent* Capsule = Player.GetCapsuleComponent();
                for (const int32 Sign : {-1, 1})
                {
                    const FVector Candidate = Player.GetActorLocation() + Right * 24.0 * Sign;
                    FHitResult Hit;
                    if (Level && SafeCombatGround(**Level, Player, Candidate)
                        && !Player.GetWorld()->SweepSingleByChannel(Hit, Player.GetActorLocation(), Candidate, FQuat::Identity,
                            ECC_Pawn, FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight()), SightQuery, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
                    {
                        Strafe = Sign;
                        break;
                    }
                }
            }
            SetCombatStrafe(Player, Strafe);
            const double Distance = FVector::Dist2D(Player.GetActorLocation(), Target);
            const bool bExiguus = Boss.GetEncounterName() == TEXT("Exiguus");
            const bool bElymas = Boss.GetEncounterName() == TEXT("Elymas");
            const bool bSkeleton = Boss.GetEncounterName() == TEXT("Skeleton Knight") || Boss.GetSourceType() == 1025;
            const bool bLegionaryOrArcher = Boss.GetSourceType() == 1011 || Boss.GetSourceType() == 1014;
            const bool bShadow = Boss.GetSourceType() == 1003;
            const bool bGargoyle = Boss.GetSourceType() == 1007;
            const bool bGriffon = Boss.GetSourceType() == 1023;
            const int32 RangedWeapon = Player.GetItemCount(902) > 0 ? 902 : 0;
            const bool bRangedArcher = Boss.GetSourceType() == 1014 && RangedWeapon != 0
                && Player.GetMana() > 500 && Distance > 96.0;
            if (Boss.GetSourceType() == 1014 && Player.GetStatusPower(NativeWadRules::ElectricityAttackStatus) == 0
                && Player.GetItemCount(370) > 0)
            {
                Stop(Player);
                if (Player.GetReadyItem() != 370 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 370); }
                else { Press(Player, EKeys::LeftControl); }
                return false;
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            if (bGriffon && NativeWadRules::SchoolForClass(Player.GetCharacterClass()) == NativeWadRules::ESpellSchool::Cleric
                && Player.HasRune(1) && Player.HasRune(2) && Player.GetMana() > 2100 && Distance > 140.0)
            {
                Stop(Player);
                SetCombatStrafe(Player, 0);
                Aim(Player, Target);
                if (Player.GetRunes().IsEmpty()) { Press(Player, EKeys::NumPadOne); }
                else if (Player.GetRunes() == TEXT("1")) { Press(Player, EKeys::NumPadTwo); }
                else if (Player.GetRunes() == TEXT("12")) { Press(Player, EKeys::RightMouseButton); }
                else { Press(Player, EKeys::BackSpace); }
                return false;
            }
            if ((bElymas || bExiguus || bLegionaryOrArcher || bGriffon) && Now >= NextElymasCombatSample)
            {
                UE_LOG(LogTemp, Display, TEXT("%s combat: boss health %d, player health %d, distance %.1f. Player %s, boss %s, ready item %d, weapon %d."),
                    *Boss.GetEncounterName(), Boss.GetEncounterHealth(), Player.GetHealth(), Distance,
                    *Player.GetActorLocation().ToCompactString(), *Target.ToCompactString(), Player.GetReadyItem(), CombatWeapon);
                UE_LOG(LogTemp, Display, TEXT("Combat aim is blocked by %s on %s. Strafe %d."),
                    *GetNameSafe(MeleeSight.GetComponent()), *GetNameSafe(MeleeSight.GetActor()), Strafe);
                NextElymasCombatSample = Now + 5.0;
            }
            // The eastern chamber has authored stairs down to the approach corridor.
            const FVector2D Location(Player.GetActorLocation());
            const FVector2D Retreat = Location + (Location - FVector2D(Target)).GetSafeNormal() * 24.0;
            const bool bExiguusRetreat = bExiguus && Distance < 61.0 && Retreat.X > -4110 && Retreat.X < -3320
                && Retreat.Y > 3480 && Retreat.Y < 3715;
            bool bElymasRetreat = false;
            if (bElymas && Distance < 61.0)
            {
                const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
                if (Level)
                {
                    const FTransform Transform = (*Level)->GetActorTransform();
                    const FVector LocalRetreat = Transform.InverseTransformPosition(FVector(Retreat, Player.GetActorLocation().Z));
                    const int32 Sector = (*Level)->FindWadSector(FVector2D(LocalRetreat));
                    FVector2D Heights;
                    const FVector Feet = Player.GetActorLocation()
                        - FVector::UpVector * Player.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                    const double LocalFeet = Transform.InverseTransformPosition(Feet).Z;
                    bElymasRetreat = (*Level)->GetSectorSurfaceHeights(Sector, Heights)
                        && FMath::Abs(Heights.X - LocalFeet) <= 24.0 && Heights.Y - Heights.X >= 56.0;
                }
            }
            const TActorIterator<ANativeWadLevel> CombatLevel(Player.GetWorld());
            const bool bGriffonRetreat = bGriffon && Distance < 84.0 && CombatLevel
                && SafeCombatGround(**CombatLevel, Player, FVector(Retreat, Player.GetActorLocation().Z));
            const bool bBack = !Boss.IsDefeated() && !bRangedArcher && Strafe == 0 && !Player.IsConcealed() && (bExiguusRetreat || bElymasRetreat || bGriffonRetreat || (bSkeleton && Distance < 43.0)
                || ((bLegionaryOrArcher || bShadow) && Distance < 61.0) || (bGargoyle && Distance < 72.0));
            if (bBack != bCombatBacking)
            {
                SendKey(Player, EKeys::S, bBack ? IE_Pressed : IE_Released);
                bCombatBacking = bBack;
                if (!bBack)
                {
                    Player.GetCharacterMovement()->StopMovementImmediately();
                }
            }
            if (Boss.IsDefeated())
            {
                SetCombatStrafe(Player, 0);
                Stop(Player);
                return true;
            }
            CombatWeapon = BestReviewMeleeWeapon(Player);
            const double Reach = bRangedArcher ? 300.0 : bGargoyle || bGriffon ? 86.0 : bExiguus || bElymas ? 77.0
                : bLegionaryOrArcher || bShadow ? 74.0 : bSkeleton ? 64.0 : 54.0;
            if (Strafe != 0) { Stop(Player); }
            else if (!bBack && Distance > Reach)
            {
                WalkTo(Player, FVector2D(Target));
            }
            else
            {
                Stop(Player);
                if (!bBack)
                {
                    // Flying has no braking deceleration. Clear input-lag drift throughout the idle frames.
                    Player.GetCharacterMovement()->StopMovementImmediately();
                }
            }
            Aim(Player, Target);
            if (Boss.GetSourceType() == 1014 && Player.GetMana() <= 100 && Player.GetItemCount(362) > 0)
            {
                if (Player.GetReadyItem() != 362 || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, 362);
                }
                else
                {
                    Press(Player, EKeys::LeftControl);
                }
                return false;
            }
            const int32 Potion = Player.GetItemCount(GreaterHealingPotion) > 0 ? GreaterHealingPotion : HealingPotion;
            if (Player.GetHealth() < Player.GetMaxHealth() / 2 && Player.GetItemCount(Potion) > 0)
            {
                if (Player.GetReadyItem() != Potion || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, Potion);
                }
                else
                {
                    Press(Player, EKeys::LeftControl);
                }
                return false;
            }
            if (Player.GetHealth() < (bGriffon ? 1000 : Player.GetMaxHealth() / 2) && Player.GetMana() >= NativeWadRules::RejuvenateCost)
            {
                CastCampaignRejuvenate(Player);
                return false;
            }
            const int32 Weapon = bRangedArcher ? RangedWeapon : CombatWeapon;
            if (Weapon > 0 && (Player.GetReadyItem() != Weapon || Player.IsInventoryOpen()))
            {
                ReadyCampaignItem(Player, Weapon);
                return false;
            }
            if (Strafe == 0 && Distance < (bRangedArcher ? 450.0 : 96.0) && (!bRangedArcher || Player.GetMana() > 300))
            {
                Press(Player, EKeys::LeftControl);
            }
            return false;
        }

        bool TickUndeadBoss(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            ANativeWadGuardian* Boss = nullptr;
            ANativeWadPickup* Orb = nullptr;
            for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
            {
                if (It->ActorHasTag(TEXT("WAD Creature Exiguus"))) { Boss = *It; break; }
            }
            for (TActorIterator<ANativeWadPickup> It(Player.GetWorld()); It; ++It)
            {
                if (It->GetItemType() == NativeWadRules::OrbOfTheUndead) { Orb = *It; break; }
            }
            if (!Boss || (NextPoint < 7 && !Orb))
            {
                return Finish(&Player, TEXT("the L12 encounter lost Exiguus or his orb"));
            }
            if (NextPoint == 0)
            {
                if (Orb->IsAvailable() || Boss->GetEncounterHealth() != 10000 || !PlaceReviewPlayer(Level, Player, {4040, 3350}))
                {
                    return Finish(&Player, TEXT("Exiguus did not begin alive with his drop hidden"));
                }
                ++NextPoint;
            }
            else if (NextPoint == 1 && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                Aim(Player, {4040, 3376, 12});
                Press(Player, EKeys::E);
                ++NextPoint;
            }
            else if (NextPoint == 2 && Player.GetItemCount(NativeWadRules::Key1) == 1
                && PlaceReviewPlayer(Level, Player, {1235, 3616}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 3 && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                Aim(Player, {1268, 3616, 60});
                Press(Player, EKeys::E);
                ++NextPoint;
            }
            else if (NextPoint == 4)
            {
                FVector2D DoorHeights;
                if (Level.GetSectorSurfaceHeights(81, DoorHeights) && DoorHeights.Y - DoorHeights.X >= 56
                    && WalkTo(Player, {1310, 3616}) && PlaceReviewPlayer(Level, Player, {-3729, 3603}))
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L12's original Key 1 opens door sector 81 and permits crossing."));
                    ++NextPoint;
                }
            }
            else if (NextPoint == 5 && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                if (FParse::Param(FCommandLine::Get(), TEXT("WadReviewPlayerCombat")))
                {
                    // Pass the northern edge of the chamber and use the original plate to halt the generators.
                    static const FVector2D Approach[] = {{-3724, 3670}, {-3806, 3671}, {-3895, 3671}};
                    if (BossPreparationPoint < UE_ARRAY_COUNT(Approach))
                    {
                        BossPreparationPoint += WalkTo(Player, Approach[BossPreparationPoint]) ? 1 : 0;
                        return true;
                    }
                    if (FightCampaignEncounter(Player, *Boss)) { ++NextPoint; }
                    return true;
                }
                Aim(Player, Boss->GetActorLocation());
                bool bSaved = false;
                if (CaptureTownFrame(TEXT("exiguus"), bSaved))
                {
                    if (!bSaved || !Boss->TakeMeleeHit(Player, Boss->GetEncounterHealth()))
                    {
                        return Finish(&Player, TEXT("the controlled Exiguus defeat failed"));
                    }
                    ++NextPoint;
                }
            }
            else if (NextPoint == 6)
            {
                if (!Boss->IsDefeated() || !Orb->IsAvailable())
                {
                    return Finish(&Player, TEXT("Exiguus did not drop the Orb of the Undead on defeat"));
                }
                if (FParse::Param(FCommandLine::Get(), TEXT("WadReviewPlayerCombat"))
                    && FVector::DistSquared2D(Player.GetActorLocation(), Orb->GetActorLocation()) > FMath::Square(42.0))
                {
                    WalkTo(Player, FVector2D(Orb->GetActorLocation()));
                    return true;
                }
                Stop(Player);
                Aim(Player, Orb->GetActorLocation());
                Press(Player, EKeys::E);
                ++NextPoint;
            }
            else if (NextPoint == 7)
            {
                if (Player.GetItemCount(NativeWadRules::OrbOfTheUndead) != 1)
                {
                    return Finish(&Player, TEXT("E did not collect Exiguus's orb"));
                }
                bool bSaved = false;
                const bool bHeadless = FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
                if (bHeadless || CaptureTownFrame(TEXT("orb-of-the-undead"), bSaved))
                {
                    if ((!bSaved && !bHeadless) || !Player.SaveWadCheckpoint(UndeadOrbSlot))
                    {
                        return Finish(&Player, TEXT("could not save Exiguus's defeat and the collected orb"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified Exiguus's defeat releases his hidden orb and E collects it."));
                    return Finish(&Player);
                }
            }
            return true;
        }

        bool TickUndeadExit(ANativeWadLevel& Level, ANativeWadPlayer& Player, const UNativeWadTravelState& Travel)
        {
            FVector2D Exit;
            Level.GetSectorSurfaceHeights(294, Exit);
            if (NextPoint == 0)
            {
                const bool bSuccess = UndeadReview == EUndeadReview::Return;
                if ((Player.GetItemCount(NativeWadRules::OrbOfTheUndead) == 1) != bSuccess)
                {
                    return Finish(&Player, TEXT("the L12 return review has the wrong orb inventory"));
                }
                AndrewStartingGold = Travel.Quest.Gold;
                ExperienceBeforeTravel = Player.GetExperience();
                if (PlaceReviewPlayer(Level, Player, {-3853, 3657})) { ++NextPoint; }
            }
            else if (NextPoint == 1 && WalkTo(Player, {-3895, 3671}))
            {
                ++NextPoint;
            }
            else if (NextPoint == 2 && Exit.X == 0 && PlaceReviewPlayer(Level, Player, {1946, 960}))
            {
                FWadLevelCheckpoint Snapshot;
                if (!Level.WriteCheckpoint(Snapshot, true) || Snapshot.CreatureGenerators.Num() != 4
                    || !Snapshot.CreatureGenerators.FindRef(0).bPaused || !Snapshot.CreatureGenerators.FindRef(1).bPaused
                    || !Snapshot.CreatureGenerators.FindRef(2).bPaused || !Snapshot.CreatureGenerators.FindRef(3).bPaused)
                {
                    return Finish(&Player, TEXT("L12's exit plate did not pause the four original skeleton generators"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified L12's exit plate lowers floor 294 to zero."));
                ++NextPoint;
            }
            else if (NextPoint == 3)
            {
                WalkTo(Player, {1880, 960});
            }
            return true;
        }

        bool TickUndead(UWorld& Current, ANativeWadPlayer& Player)
        {
            const auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the Exiguus review lost the player or campaign"));
            }
            const bool bTown = Current.GetAuthGameMode<ANativeWadTownGameMode>() != nullptr;
            const FWadQuestProgress& Quest = Travel->Quest;
            if (bTown && UndeadReview == EUndeadReview::Launch)
            {
                if (!Player.HasRestoredWadCheckpoint() || !Player.HasCompletedWadLevel(TEXT("L153")))
                {
                    return Finish(&Player, TEXT("the Exiguus quest needs the completed Elmore save"));
                }
                if (Quest.bShowResult) { Press(Player, EKeys::Enter); return true; }
                if (NextPoint == 0 && Quest.Selected == 2)
                {
                    RecordTownCharacter(Player);
                    Press(Player, EKeys::Right);
                    ++NextPoint;
                }
                else if (NextPoint == 1 && Quest.Selected == 3)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::Q);
                    ++NextPoint;
                }
                else if (NextPoint == 2 && CaptureUndeadFrame(Player, TEXT("briefing")))
                {
                    Press(Player, EKeys::Enter);
                    ++NextPoint;
                }
                return true;
            }
            if (bTown)
            {
                if (Travel->ArrivingFromMap != 0) { return true; }
                const bool bSuccess = UndeadReview == EUndeadReview::Return;
                const auto* L12 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L12"));
                if (!Quest.bShowResult || Quest.Active != INDEX_NONE || Quest.ResultQuest != 3 || Quest.bSucceeded != bSuccess
                    || Quest.Gold != AndrewStartingGold + (bSuccess ? 50 : 0) || Player.GetExperience() != ExperienceBeforeTravel
                    || Player.GetItemCount(NativeWadRules::OrbOfTheUndead) != 0
                    || !L12 || L12->RemainingPickups.Contains(TEXT("WAD Exiguus Orb Drop")) == bSuccess)
                {
                    return Finish(&Player, TEXT("Town Hall did not apply the original Orb of the Undead result"));
                }
                if (CaptureUndeadFrame(Player, bSuccess ? TEXT("town-success") : TEXT("town-empty")))
                {
                    if (bSuccess && !Player.SaveWadCheckpoint(UndeadTownSlot))
                    {
                        return Finish(&Player, TEXT("could not save the completed Exiguus quest"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified Exiguus Town Hall result. Success=%d, gold reward=%d."),
                        bSuccess, Quest.Gold - AndrewStartingGold);
                    return Finish(&Player);
                }
                return true;
            }
            if (Quest.Active != 3 || !Travel->JournalPages.Contains(403))
            {
                return Finish(&Player, TEXT("the Exiguus route lost its quest or source journal page"));
            }
            if (&Current != SourceWorld.Get())
            {
                const TCHAR* Map = UndeadReview == EUndeadReview::Launch ? TEXT("L231") : UndeadReview == EUndeadReview::Castle ? TEXT("L75")
                    : UndeadReview == EUndeadReview::Traps ? TEXT("L10") : TEXT("L12");
                const FVector2D Spawn = UndeadReview == EUndeadReview::Launch ? FVector2D(-2208, 2416)
                    : UndeadReview == EUndeadReview::Castle ? FVector2D(2096, 3034)
                    : UndeadReview == EUndeadReview::Traps ? FVector2D(-1268, -531) : FVector2D(4530, 1171);
                const TCHAR* Slot = UndeadReview == EUndeadReview::Launch ? UndeadCastleSlot : UndeadReview == EUndeadReview::Castle ? UndeadTrapsSlot
                    : UndeadReview == EUndeadReview::Traps ? UndeadKeepSlot : UndeadCemeterySlot;
                if (!Current.GetMapName().EndsWith(Map) || !FVector2D(Player.GetActorLocation()).Equals(Spawn, 4)
                    || (UndeadReview == EUndeadReview::Launch && !MatchesTownCharacter(Player)))
                {
                    return Finish(&Player, TEXT("the Exiguus route did not reach its authored start"));
                }
                if (Player.GetCharacterMovement()->IsMovingOnGround() && CaptureUndeadFrame(Player, TEXT("arrival")))
                {
                    if (!Player.SaveWadCheckpoint(Slot))
                    {
                        return Finish(&Player, TEXT("could not save the Exiguus arrival"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified Exiguus quest arrival at %s with the source spawn and journal."), Map);
                    return Finish(&Player);
                }
                return true;
            }
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || !Player.HasRestoredWadCheckpoint())
            {
                return Finish(&Player, TEXT("the Exiguus map needs its native checkpoint"));
            }
            switch (UndeadReview)
            {
            case EUndeadReview::Castle: return TickUndeadCastle(**Level, Player);
            case EUndeadReview::Traps: return TickUndeadTraps(**Level, Player);
            case EUndeadReview::Keep: return TickUndeadKeep(**Level, Player);
            case EUndeadReview::Boss: return TickUndeadBoss(**Level, Player);
            default: return TickUndeadExit(**Level, Player, *Travel);
            }
        }

        bool CaptureUndeadFrame(ANativeWadPlayer& Player, const TCHAR* Name)
        {
            bool bSaved = false;
            if (!CaptureTownFrame(Name, bSaved))
            {
                return false;
            }
            if (!bSaved)
            {
                UE_LOG(LogTemp, Warning, TEXT("Could not capture Exiguus review image %s in %s."), Name, *Player.GetWorld()->GetMapName());
            }
            return true;
        }

        bool TickTempleL51(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            FVector2D Gate;
            if (!Level.GetSectorSurfaceHeights(115, Gate))
            {
                return Finish(&Player, TEXT("L51 is missing the authored exit gate"));
            }
            switch (NextPoint)
            {
            case 0:
                if (Gate != FVector2D(8, 16) || !PlaceReviewPlayer(Level, Player, {598, 2016}))
                {
                    return Finish(&Player, TEXT("could not reach L51's poison corridor with the exit gate closed"));
                }
                TrapHealth = Player.GetHealth();
                PoisonBeforeTrap = Player.GetPoisonLevel();
                ++NextPoint;
                break;
            case 1:
                if (WalkTo(Player, {545, 2020}))
                {
                    ++NextPoint;
                }
                break;
            case 2:
                if (Level.GetScriptMissileCount(NativeWadRules::PoisonCrossbowBolt) == 2
                    && Player.GetHealth() <= TrapHealth - 300 && Player.GetPoisonLevel() > PoisonBeforeTrap)
                {
                    bool bSaved = false;
                    Aim(Player, {746, 2020, 28});
                    if (CaptureTownFrame(TEXT("poison-corridor"), bSaved))
                    {
                        if (!bSaved || !PlaceReviewPlayer(Level, Player, {-1800, 2202}))
                        {
                            return Finish(&Player, TEXT("could not capture the poison corridor or reach the gate switch"));
                        }
                        UE_LOG(LogTemp, Display, TEXT("Verified L51 fires two authored type 887 bolts and poisons the player."));
                        ++NextPoint;
                    }
                }
                break;
            case 3:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, {-1833, 2202, 245});
                    Press(Player, EKeys::E);
                    ++NextPoint;
                }
                break;
            case 4:
                if (Gate.Y == 168)
                {
                    if (!PlaceReviewPlayer(Level, Player, {-1829, 3642}))
                    {
                        return Finish(&Player, TEXT("could not reach the opened exit gate"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L51 switch 674 lifts gate 115 from Z16 to Z168."));
                    ++NextPoint;
                }
                break;
            case 5:
                WalkTo(Player, {-2272, 3642});
                break;
            }
            return true;
        }

        bool TickTempleL52(ANativeWadLevel& Level, ANativeWadPlayer& Player)
        {
            struct FPlate
            {
                FVector2D Approach, Target;
                int32 Sector;
                double GateHeight;
            };
            const FPlate Plates[] = {
                {{-1878, 3552}, {-1830, 3552}, 96, 25},
                {{-1388, 3630}, {-1360, 3670}, 106, 50},
                {{-976, 3280}, {-944, 3248}, 97, 75},
            };
            FVector2D Gate;
            if (!Level.GetSectorSurfaceHeights(94, Gate))
            {
                return Finish(&Player, TEXT("L52 is missing the three-plate gate"));
            }
            switch (NextPoint)
            {
            case 0:
                if (Gate.Y != 0 || !PlaceReviewPlayer(Level, Player, {-2192, -1258}))
                {
                    return Finish(&Player, TEXT("L52 must start with the plate gate shut and the pit approach accessible"));
                }
                ++NextPoint;
                break;
            case 1:
                if (FVector2D(Player.GetActorLocation()).Equals({-1536, 1816}, 4))
                {
                    Stop(Player);
                    UE_LOG(LogTemp, Display, TEXT("Verified the L52 pit teleporter rescues the player to (-1536, 1816)."));
                    ++NextPoint;
                }
                else
                {
                    WalkTo(Player, {-2192, -1304});
                }
                break;
            case 2:
                if (!PlaceReviewPlayer(Level, Player, Plates[TemplePlate].Approach))
                {
                    return Finish(&Player, TEXT("could not reach an L52 pressure plate approach"));
                }
                ++NextPoint;
                break;
            case 3:
                if (WalkTo(Player, Plates[TemplePlate].Target))
                {
                    if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) != Plates[TemplePlate].Sector)
                    {
                        return Finish(&Player, TEXT("the player did not step onto the requested L52 plate"));
                    }
                    ++NextPoint;
                }
                break;
            case 4:
                if (Gate.Y == Plates[TemplePlate].GateHeight)
                {
                    ++TemplePlate;
                    NextPoint = TemplePlate < UE_ARRAY_COUNT(Plates) ? 2 : 5;
                }
                break;
            case 5:
                if (!PlaceReviewPlayer(Level, Player, {-1536, 2858}))
                {
                    return Finish(&Player, TEXT("could not reach the open L52 gate"));
                }
                ++NextPoint;
                break;
            case 6:
            {
                bool bSaved = false;
                Aim(Player, {-1536, 2912, 35});
                if (CaptureTownFrame(TEXT("three-plate-gate"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the open L52 gate"));
                    }
                    ++NextPoint;
                }
                break;
            }
            case 7:
                if (WalkTo(Player, {-1536, 2966}))
                {
                    if (!PlaceReviewPlayer(Level, Player, {-3402, 1440}))
                    {
                        return Finish(&Player, TEXT("could not reach the L52 exit approach after crossing the gate"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L52's three plates raise gate 94 to Z25, Z50, and Z75 and allow crossing."));
                    ++NextPoint;
                }
                break;
            case 8:
                if (Gate.Y == 0)
                {
                    FWadLevelCheckpoint Snapshot;
                    if (!Level.WriteCheckpoint(Snapshot) || Snapshot.Variables[1] != 0 || Snapshot.Variables[2] != 0
                        || Snapshot.Variables[3] != 0 || Snapshot.Variables[4] != 0)
                    {
                        return Finish(&Player, TEXT("L52 did not reset the plates and gate after the source delay"));
                    }
                    ++NextPoint;
                }
                break;
            case 9:
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    const FAAWadNativeLevelLink* Link = Level.GetLevelLinks().FindByPredicate([](const auto& Entry)
                    {
                        return Entry.SourceThing == 238;
                    });
                    if (!Link || Link->Sector != 170 || Link->Destination.ToSoftObjectPath().GetLongPackageName() != TEXT("/Game/Maps/L41")
                        || !Player.SaveWadCheckpoint(TempleExitSlot))
                    {
                        return Finish(&Player, TEXT("could not verify the original L41 exit or save its approach"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L52 resets its plates and saves before Thing 238, the original L41 exit."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool CollectCampaignItem(ANativeWadPlayer& Player, int32 Type, const FVector& Position, bool bAirborne = false)
        {
            if (CampaignPickupCount == INDEX_NONE && (Player.GetCharacterMovement()->IsMovingOnGround() || Player.CanFly() || bAirborne))
            {
                CampaignPickupCount = Player.GetItemCount(Type);
                CampaignNextUseTime = 0.0;
            }
            if (CampaignPickupCount != INDEX_NONE && Player.GetItemCount(Type) == CampaignPickupCount + 1)
            {
                return true;
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            if (CampaignPickupCount != INDEX_NONE && Now >= CampaignNextUseTime)
            {
                Aim(Player, Position);
                Press(Player, EKeys::E);
                CampaignNextUseTime = Now + 0.4;
            }
            return false;
        }

        void CastCampaignRejuvenate(ANativeWadPlayer& Player)
        {
            if (Player.GetMana() < NativeWadRules::RejuvenateCost)
            {
                return;
            }
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
                Press(Player, EKeys::RightMouseButton);
            }
        }

        void ReadyCampaignItem(ANativeWadPlayer& Player, int32 Type)
        {
            if (Player.IsInventoryOpen())
            {
                const int32 Slot = Player.GetCarriedItems().Find(Type);
                APlayerController* Controller = Player.GetController<APlayerController>();
                ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
                if (Slot != INDEX_NONE && FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
                {
                    // Without a canvas the HUD has no hit targets. Use its inventory handlers directly.
                    Player.ClickInventorySlot(Slot);
                    Player.ClickInventorySlot(INDEX_NONE);
                }
                else if (Hud && Slot != INDEX_NONE)
                {
                    Hud->ClickHUD(Hud->InventorySlotCenter(Slot));
                    Hud->ClickHUD(Hud->InventorySlotCenter(INDEX_NONE));
                }
            }
            Press(Player, EKeys::I);
        }

        bool TickCampaignAction(ANativeWadLevel& Level, ANativeWadPlayer& Player, const FCampaignAction& Step)
        {
            switch (Step.Action)
            {
            case ECampaignAction::Place:
                return PlaceReviewPlayer(Level, Player, FVector2D(Step.Target));
            case ECampaignAction::Walk:
            {
                if (ANativeWadGuardian* Blocking = WalkBlockingEnemy.Get(); Blocking && Blocking->GetWorld() == Player.GetWorld()
                    && !Blocking->IsDefeated())
                {
                    FightCampaignEncounter(Player, *Blocking);
                    CampaignStepTime = -1.0;
                    return false;
                }
                WalkBlockingEnemy.Reset();
                const bool bElmore = CampaignRoute->Quest == 2;
                const bool bRangedApproach = CampaignRoute->Map == 41 || CampaignRoute->Map == 70 || CampaignRoute->Map == 71
                    || CampaignRoute->Map == 61 || CampaignRoute->Map == 40;
                const int32 Potion = Player.GetItemCount(GreaterHealingPotion) > 0 ? GreaterHealingPotion : HealingPotion;
                const int32 HealBelow = CampaignRoute->Map == 73 || CampaignRoute->Map == 51 || CampaignRoute->Map == 44 || bRangedApproach
                    ? FMath::Min(2300, Player.GetMaxHealth() * 3 / 4)
                    : Potion == GreaterHealingPotion ? 1050 : 1300;
                const bool bNeedsHealing = Player.GetHealth() < HealBelow;
                const bool bUsePotion = bElmore || ((bNeedsHealing || Player.IsInventoryOpen())
                    && (CampaignRoute->Quest == 3 || CampaignRoute->Quest == 5 || CampaignRoute->Map == 51 || CampaignRoute->Map == 44 || bRangedApproach
                        || FParse::Param(FCommandLine::Get(), TEXT("WadReviewPlayerCombat"))));
                if (bUsePotion && Player.GetItemCount(Potion) > 0)
                {
                    if (Player.GetReadyItem() != Potion || Player.IsInventoryOpen())
                    {
                        ReadyCampaignItem(Player, Potion);
                        return false;
                    }
                    if (bNeedsHealing)
                    {
                        Press(Player, EKeys::LeftControl);
                    }
                }
                else if ((CampaignRoute->Map == 20 || CampaignRoute->Map == 60 || CampaignRoute->Quest == 3
                    || CampaignRoute->Quest == 5 || bElmore || bRangedApproach)
                    && (Player.GetHealth() < (bRangedApproach ? HealBelow : CampaignRoute->Map == 34 ? 1450 : 1050)
                        || !Player.GetRunes().IsEmpty()))
                {
                    CastCampaignRejuvenate(Player);
                }
                if (CampaignRoute->Map == 73 && Player.CanFly())
                {
                    FVector2D Heights;
                    if (Level.GetSectorSurfaceHeights(Level.FindWadSector(FVector2D(Player.GetActorLocation())), Heights))
                    {
                        const double Desired = Level.GetActorLocation().Z + FMath::Min(Heights.X + ANativeWadPlayer::CapsuleHalfHeight + 90.0,
                            Heights.Y - ANativeWadPlayer::CapsuleHalfHeight - 2.0);
                        FVector2D AheadHeights;
                        const FVector2D Position(Player.GetActorLocation());
                        const FVector2D Ahead = Position + (FVector2D(Step.Target) - Position).GetClampedToMaxSize(48.0);
                        const double SafeDesired = Level.GetSectorSurfaceHeights(Level.FindWadSector(Ahead), AheadHeights)
                            ? FMath::Min(Desired, Level.GetActorLocation().Z + AheadHeights.Y - ANativeWadPlayer::CapsuleHalfHeight - 2.0) : Desired;
                        const double Difference = SafeDesired - Player.GetActorLocation().Z;
                        SetFlightThrust(Player, Difference > 5.0);
                        if (FMath::Abs(Difference) > 24.0) { Stop(Player); return false; }
                    }
                }
                if (CampaignRoute->Map == 70 || CampaignRoute->Map == 71)
                {
                    if (CampaignObstacleDetour)
                    {
                        if (WalkTo(Player, CampaignObstacleDetour.GetValue()))
                        {
                            CampaignObstacleDetour = CampaignObstacleAfter;
                            CampaignObstacleAfter.Reset();
                        }
                        return false;
                    }
                    const FVector Start = Player.GetActorLocation();
                    const FVector End(Step.Target.X, Step.Target.Y, Start.Z);
                    const UCapsuleComponent* Capsule = Player.GetCapsuleComponent();
                    const FCollisionResponseParams Responses(Capsule->GetCollisionResponseToChannels());
                    const FCollisionShape Shape = FCollisionShape::MakeCapsule(
                        Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
                    FCollisionQueryParams Query(SCENE_QUERY_STAT(CampaignSceneryDetour), false, &Player);
                    Query.bFindInitialOverlaps = false;
                    FHitResult Obstacle;
                    if (Player.GetWorld()->SweepSingleByChannel(Obstacle, Start, End, FQuat::Identity, ECC_Pawn, Shape, Query, Responses)
                        && Obstacle.Distance < 96.0
                        && (Cast<ANativeWadScenery>(Obstacle.GetActor())
                            || ((CampaignRoute->Map == 71 || CampaignRoute->Map == 70) && Cast<ANativeWadGuardian>(Obstacle.GetActor()))))
                    {
                        if ((CampaignRoute->Map == 71 || CampaignRoute->Map == 70) && Cast<ANativeWadGuardian>(Obstacle.GetActor())
                            && FVector::DistSquared2D(Start, End) < FMath::Square(40.0))
                        {
                            UE_LOG(LogTemp, Display, TEXT("The concealed tower replay continues from beside the archer occupying waypoint %s."),
                                *End.ToCompactString());
                            return true;
                        }
                        const FVector Direction = (End - Start).GetSafeNormal2D();
                        const FVector Side(-Direction.Y, Direction.X, 0);
                        const FVector Extent = Obstacle.GetComponent()->Bounds.BoxExtent;
                        const bool bCreature = Cast<ANativeWadGuardian>(Obstacle.GetActor()) != nullptr;
                        const double Clearance = (bCreature ? 1.0 : 2.0) * (FMath::Max(Extent.X, Extent.Y) + Capsule->GetScaledCapsuleRadius()) + 12.0;
                        const double ForwardClearance = bCreature ? Obstacle.Distance + 2.0 * Clearance : Clearance;
                        for (const double Sign : {1.0, -1.0})
                        {
                            const FVector Detour = Start + Side * Clearance * Sign;
                            const FVector After = Detour + Direction * ForwardClearance;
                            FHitResult Hit;
                            if (SafeCombatGround(Level, Player, Detour) && SafeCombatGround(Level, Player, After)
                                && !Player.GetWorld()->SweepSingleByChannel(Hit, Start, Detour, FQuat::Identity, ECC_Pawn, Shape, Query, Responses)
                                && !Player.GetWorld()->SweepSingleByChannel(Hit, Detour, After, FQuat::Identity, ECC_Pawn, Shape, Query, Responses))
                            {
                                CampaignObstacleDetour = FVector2D(Detour);
                                CampaignObstacleAfter = FVector2D(After);
                                UE_LOG(LogTemp, Display, TEXT("The campaign replay walks around obstacle %s via %s."),
                                    *Obstacle.GetActor()->GetActorNameOrLabel(), *Detour.ToCompactString());
                                return false;
                            }
                        }
                        if (auto* Blocking = Cast<ANativeWadGuardian>(Obstacle.GetActor()))
                        {
                            WalkBlockingEnemy = Blocking;
                            Stop(Player);
                            FightCampaignEncounter(Player, *Blocking);
                            CampaignStepTime = -1.0;
                            return false;
                        }
                    }
                }
                return WalkTo(Player, FVector2D(Step.Target));
            }
            case ECampaignAction::TouchSector:
                if (Level.FindWadSector(FVector2D(Player.GetActorLocation())) == Step.Index)
                {
                    Stop(Player);
                    return true;
                }
                WalkTo(Player, FVector2D(Step.Target));
                return false;
            case ECampaignAction::Teleport:
            {
                const FVector2D Location(Player.GetActorLocation());
                const bool bArrived = Step.Index > 0 ? FVector2D::DistSquared(Location, Step.Destination) <= FMath::Square(double(Step.Index))
                    : Location.Equals(Step.Destination, 6.0);
                if (bArrived)
                {
                    Stop(Player);
                    return true;
                }
                WalkTo(Player, FVector2D(Step.Target));
                return false;
            }
            case ECampaignAction::WaitBoulder:
            {
                Stop(Player);
                const FName Tag(*FString::Printf(TEXT("WAD Creature Thing %d"), Step.Index));
                for (TActorIterator<ANativeWadGuardian> Hazard(Player.GetWorld()); Hazard; ++Hazard)
                {
                    if (Hazard->ActorHasTag(Tag))
                    {
                        const double X = Hazard->GetActorLocation().X;
                        return X >= Step.Target.X && X <= Step.Target.Y;
                    }
                }
                return true;
            }
            case ECampaignAction::FireWand:
            {
                using namespace NativeWadRules;
                if (!FParse::Param(FCommandLine::Get(), TEXT("WadReviewFireWand")))
                {
                    return true;
                }
                ANativeWadGuardian* Apollo = nullptr;
                for (TActorIterator<ANativeWadGuardian> Target(Player.GetWorld()); Target; ++Target)
                {
                    if (Target->ActorHasTag(TEXT("WAD Creature Thing 29")))
                    {
                        Apollo = *Target;
                        break;
                    }
                }
                if (!Apollo)
                {
                    return false;
                }
                if (bCampaignActionStarted)
                {
                    if (Apollo->GetEncounterHealth() != HealthBeforeCast - PlasmaDamage / 2
                        || Player.GetItemCount(Step.Index) != 1 || Player.GetMana() > ManaBeforeTravel - DisintegrationManaCost)
                    {
                        return false;
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the L72 wand dealt %d damage to the fire-resistant Apollo."),
                        PlasmaDamage / 2);
                    return true;
                }
                if (Player.GetReadyItem() != Step.Index || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, Step.Index);
                    return false;
                }
                if (Player.GetMana() <= DisintegrationManaCost)
                {
                    return false;
                }
                UE_LOG(LogTemp, Display, TEXT("The Disintegration review fires from %.1f horizontal units away from Apollo."),
                    FVector::Dist2D(Player.GetActorLocation(), Apollo->GetActorLocation()));
                ManaBeforeTravel = Player.GetMana();
                HealthBeforeCast = Apollo->GetEncounterHealth();
                Aim(Player, Apollo->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                bCampaignActionStarted = true;
                return false;
            }
            case ECampaignAction::UseFlightItem:
                if (bCampaignActionStarted)
                {
                    return Player.CanFly();
                }
                if (Player.GetReadyItem() != Step.Index || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, Step.Index);
                    return false;
                }
                if (Step.Index == NativeWadRules::FlyingAmulet)
                {
                    return Player.CanFly();
                }
                Press(Player, EKeys::LeftControl);
                bCampaignActionStarted = true;
                return false;
            case ECampaignAction::FlightEnded:
                return !Player.CanFly() && Player.GetCharacterMovement()->IsMovingOnGround();
            case ECampaignAction::Fly:
            {
                const int32 HealBelow = CampaignRoute->Map == 72 ? Player.GetMaxHealth() * 3 / 4 : 2300;
                if ((CampaignRoute->Map == 72 || CampaignRoute->Map == 73 || CampaignRoute->Map == 44)
                    && (Player.GetHealth() < HealBelow || Player.IsInventoryOpen()))
                {
                    const int32 Potion = Player.GetItemCount(GreaterHealingPotion) > 0 ? GreaterHealingPotion : HealingPotion;
                    if (Player.GetItemCount(Potion) > 0)
                    {
                        if (Player.GetReadyItem() != Potion || Player.IsInventoryOpen())
                        {
                            ReadyCampaignItem(Player, Potion);
                        }
                        else
                        {
                            Press(Player, EKeys::LeftControl);
                        }
                        return false;
                    }
                }
                if (!Player.CanFly())
                {
                    return false;
                }
                const double Difference = Step.Target.Z - Player.GetActorLocation().Z;
                SetFlightThrust(Player, Difference > 12.0);
                if (FMath::Abs(Difference) > 30.0)
                {
                    Stop(Player);
                    return false;
                }
                if (Step.Index > 0 && FVector2D::Distance(FVector2D(Player.GetActorLocation()), FVector2D(Step.Target)) <= Step.Index)
                {
                    Stop(Player);
                    SetFlightThrust(Player, false);
                    return true;
                }
                if (!WalkTo(Player, FVector2D(Step.Target)))
                {
                    if (CampaignRoute->Map == 72 || CampaignRoute->Map == 73)
                    {
                        const UCapsuleComponent* Capsule = Player.GetCapsuleComponent();
                        const FVector Start = Player.GetActorLocation();
                        const FVector Direction = (FVector(Step.Target.X, Step.Target.Y, Start.Z) - Start).GetSafeNormal();
                        FCollisionQueryParams Query(SCENE_QUERY_STAT(CampaignFlyingPassage), false, &Player);
                        Query.bFindInitialOverlaps = false;
                        FHitResult Hit;
                        Player.GetWorld()->SweepSingleByChannel(Hit, Start, Start + Direction * 32.0, FQuat::Identity,
                            ECC_Pawn, FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),
                                Capsule->GetScaledCapsuleHalfHeight()), Query);
                        if (auto* Blocker = Cast<ANativeWadGuardian>(Hit.GetActor());
                            Blocker && !Blocker->IsDefeated())
                        {
                            Aim(Player, Blocker->GetActorLocation() + FVector(0, 0, Blocker->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.35));
                            CampaignStepTime = -1.0;
                            const int32 Weapon = BestReviewMeleeWeapon(Player);
                            if (Player.GetReadyItem() != Weapon || Player.IsInventoryOpen())
                            {
                                ReadyCampaignItem(Player, Weapon);
                            }
                            else
                            {
                                Press(Player, EKeys::LeftControl);
                            }
                        }
                    }
                    return false;
                }
                SetFlightThrust(Player, false);
                return true;
            }
            case ECampaignAction::Jump:
            {
                if ((CampaignRoute->Map == 73 || CampaignRoute->Map == 51) && Player.GetItemCount(GreaterHealingPotion) > 0
                    && (Player.GetHealth() < 2300 || Player.IsInventoryOpen()))
                {
                    if (Player.GetReadyItem() != GreaterHealingPotion || Player.IsInventoryOpen())
                    {
                        ReadyCampaignItem(Player, GreaterHealingPotion);
                    }
                    else
                    {
                        Press(Player, EKeys::LeftControl);
                    }
                    return false;
                }
                const FVector2D Target(Step.Target);
                const bool bOnGround = Player.GetCharacterMovement()->IsMovingOnGround();
                const bool bAtTarget = WalkTo(Player, Target);
                // Keep the running speed and jump when the remaining distance reaches the takeoff range.
                const double Now = Level.GetWorld()->GetTimeSeconds();
                const bool bRetryJump = CampaignRoute->Map == 71 || CampaignRoute->Map == 153;
                if ((!bCampaignActionStarted || (bRetryJump && !bAtTarget && Now - LastCampaignJumpTime > 1.0))
                    && bOnGround
                    && FVector2D::DistSquared(Target, FVector2D(Player.GetActorLocation())) <= FMath::Square(Step.Index))
                {
                    Press(Player, EKeys::SpaceBar);
                    if (!bCampaignActionStarted) { CampaignJumpPeak = Player.GetActorLocation().Z; }
                    bCampaignActionStarted = true;
                    LastCampaignJumpTime = Now;
                    return false;
                }
                if (bCampaignActionStarted) { CampaignJumpPeak = FMath::Max(CampaignJumpPeak, Player.GetActorLocation().Z); }
                return bAtTarget && bOnGround;
            }
            case ECampaignAction::CornerJump:
            {
                const FVector2D ToCorner = Step.Destination - FVector2D(Player.GetActorLocation());
                if (FVector2D::DotProduct(ToCorner, FVector2D(Step.Target) - Step.Destination) <= 0.0)
                {
                    return WalkTo(Player, FVector2D(Step.Target)) && Player.GetCharacterMovement()->IsMovingOnGround();
                }
                Player.GetController()->SetControlRotation(FVector(ToCorner, 0).Rotation());
                Walk(Player);
                if (!bCampaignActionStarted && ToCorner.SizeSquared() <= FMath::Square(Step.Index)
                    && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Press(Player, EKeys::SpaceBar);
                    bCampaignActionStarted = true;
                }
                return false;
            }
            case ECampaignAction::Use:
                if (!Player.GetCharacterMovement()->IsMovingOnGround() && !Player.CanFly())
                {
                    return false;
                }
                Aim(Player, Step.Target);
                Press(Player, EKeys::E);
                return true;
            case ECampaignAction::UseLockedDoor:
            {
                FVector2D Heights;
                if (Level.GetSectorSurfaceHeights(Step.Index, Heights) && Heights.Y - Heights.X >= 56.0)
                {
                    return true;
                }
                const double Now = Level.GetWorld()->GetTimeSeconds();
                if (!bCampaignActionStarted || Now >= CampaignNextUseTime)
                {
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    CampaignNextUseTime = Now + 0.4;
                    bCampaignActionStarted = true;
                }
                return false;
            }
            case ECampaignAction::CrossDoor:
            {
                if (Player.CanFly() && FVector2D::DistSquared(FVector2D(Player.GetActorLocation()), Step.Destination) > FMath::Square(64.0)
                    && FVector2D::DistSquared(FVector2D(Player.GetActorLocation()), FVector2D(Step.Target)) > FMath::Square(64.0))
                {
                    const FCampaignAction Approach{ECampaignAction::Walk, FVector(Step.Destination, 0.0)};
                    TickCampaignAction(Level, Player, Approach);
                    return false;
                }
                double DoorAimZ = Step.Target.Z;
                if (Player.CanFly())
                {
                    FVector2D Limits;
                    if (CampaignRoute->Map == 73 && Step.Index == 18) { DoorAimZ += 40.0; }
                    if (Level.GetSectorSurfaceHeights(Level.FindWadSector(FVector2D(Player.GetActorLocation())), Limits))
                    {
                        DoorAimZ = FMath::Min(DoorAimZ, Level.GetActorLocation().Z + Limits.Y - ANativeWadPlayer::CapsuleHalfHeight - 2.0);
                    }
                    const double Difference = DoorAimZ - Player.GetActorLocation().Z;
                    SetFlightThrust(Player, Difference > 5.0);
                    if (FMath::Abs(Difference) > 12.0) { Stop(Player); return false; }
                }
                const FVector2D FarSide(Step.Target);
                const FVector2D DoorCenter = (FarSide + Step.Destination) * 0.5;
                const FVector2D Across = (FarSide - Step.Destination).GetSafeNormal();
                const FVector2D Location(Player.GetActorLocation());
                if (FVector2D::DotProduct(Location - DoorCenter, Across) >= 8.0)
                {
                    return WalkTo(Player, FarSide);
                }

                FVector2D Heights;
                if (bCampaignActionStarted && Level.GetSectorSurfaceHeights(Step.Index, Heights)
                    && Heights.Y - Heights.X >= 65.0)
                {
                    return WalkTo(Player, FarSide);
                }
                if (!WalkTo(Player, Step.Destination))
                {
                    return false;
                }
                const double Now = Level.GetWorld()->GetTimeSeconds();
                if (!bCampaignActionStarted || Now >= CampaignNextUseTime)
                {
                    Aim(Player, FVector(DoorCenter - Across * 8.0, DoorAimZ));
                    Press(Player, EKeys::E);
                    CampaignNextUseTime = Now + 0.4;
                    bCampaignActionStarted = true;
                }
                return false;
            }
            case ECampaignAction::Floor:
            case ECampaignAction::Ceiling:
            case ECampaignAction::CeilingAtMost:
            case ECampaignAction::OpenDoor:
            {
                FVector2D Heights;
                return Level.GetSectorSurfaceHeights(Step.Index, Heights)
                    && (Step.Action == ECampaignAction::OpenDoor ? Heights.Y - Heights.X >= Step.Target.Z
                        : Step.Action == ECampaignAction::CeilingAtMost ? Heights.Y <= Step.Target.Z
                        : FMath::IsNearlyEqual(Step.Action == ECampaignAction::Floor ? Heights.X : Heights.Y, Step.Target.Z, 0.01));
            }
            case ECampaignAction::Pickup:
            {
                FVector Position = Step.Target;
                if (Position.Z == 0.0)
                {
                    // The original XY can sit beside solid scenery. Use the imported pickup's mounted height.
                    for (TActorIterator<ANativeWadPickup> Item(Player.GetWorld()); Item; ++Item)
                    {
                        if (Item->GetItemType() == Step.Index && FVector2D(Item->GetActorLocation()).Equals(FVector2D(Position), 1.0))
                        {
                            Position = Item->GetActorLocation();
                            break;
                        }
                    }
                }
                return CollectCampaignItem(Player, Step.Index, Position);
            }
            case ECampaignAction::JumpPickup:
                if (!bCampaignActionStarted)
                {
                    Press(Player, EKeys::SpaceBar);
                    bCampaignActionStarted = true;
                    return false;
                }
                if (CampaignPickupCount == INDEX_NONE && Player.GetPawnViewLocation().Z < Step.Target.Z + 10.0)
                {
                    return false;
                }
                return CollectCampaignItem(Player, Step.Index, Step.Target, true);
            case ECampaignAction::HealthAtLeast:
                return Player.GetHealth() >= Step.Target.Z;
            case ECampaignAction::ManaAtLeast:
                return Player.GetMana() >= Step.Target.Z;
            case ECampaignAction::Checkpoint:
                return Player.SaveWadCheckpoint(Step.Label);
            case ECampaignAction::StatueAngle:
            {
                FWadLevelCheckpoint State;
                if (Level.GetWorld()->GetTimeSeconds() - CampaignStepTime < 1.05 || !Level.WriteCheckpoint(State, true))
                {
                    return false;
                }
                const uint16* Angle = State.ScriptObjectAngles.Find(Step.Index);
                return Angle && *Angle == Step.Target.Z;
            }
            case ECampaignAction::Rejuvenate:
                if (Player.GetHealth() >= FMath::Min<double>(Step.Target.Z, Player.GetMaxHealth()))
                {
                    return true;
                }
                CastCampaignRejuvenate(Player);
                return false;
            case ECampaignAction::Defeat:
            {
                if (CampaignPickupCount != INDEX_NONE && Player.GetItemCount(Step.Index) == CampaignPickupCount + 1)
                {
                    return true;
                }
                ANativeWadPickup* Drop = nullptr;
                for (TActorIterator<ANativeWadPickup> It(Player.GetWorld()); It; ++It)
                {
                    if (It->GetItemType() == Step.Index)
                    {
                        Drop = *It;
                        break;
                    }
                }
                if (!Drop)
                {
                    return false;
                }
                if (!bCampaignActionStarted && !Drop->IsAvailable()
                    && (Player.GetCharacterMovement()->IsMovingOnGround() || Player.CanFly()))
                {
                    for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                    {
                        if (It->GetEncounterName() == Step.Label)
                        {
                            bCampaignActionStarted = FightCampaignEncounter(Player, **It);
                            break;
                        }
                    }
                }
                else if (Drop->IsAvailable() && !bCampaignActionStarted)
                {
                    Stop(Player);
                    if (bCombatBacking)
                    {
                        SendKey(Player, EKeys::S, IE_Released);
                        bCombatBacking = false;
                    }
                    bCampaignActionStarted = true;
                }
                if (!bCampaignActionStarted || !Drop->IsAvailable())
                {
                    return false;
                }
                if (FVector::DistSquared2D(Player.GetActorLocation(), Drop->GetActorLocation()) > FMath::Square(42.0))
                {
                    WalkTo(Player, FVector2D(Drop->GetActorLocation()));
                    return false;
                }
                Stop(Player);
                return CollectCampaignItem(Player, Step.Index, Drop->GetActorLocation());
            }
            case ECampaignAction::DefeatCreature:
            {
                const FName Tag(*FString::Printf(TEXT("WAD Creature Thing %d"), Step.Index));
                for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                {
                    if (It->ActorHasTag(Tag)) { return FightCampaignEncounter(Player, **It); }
                }
                return false;
            }
            case ECampaignAction::Frame:
            {
                if (FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
                {
                    return true;
                }
                bool bSaved = false;
                Aim(Player, Step.Target);
                return CaptureTownFrame(Step.Label, bSaved) && bSaved;
            }
            }
            return false;
        }

        bool TickCampaign(UWorld& Current, ANativeWadPlayer& Player)
        {
            const double FrameTime = FPlatformTime::Seconds();
            if (CampaignRoute && CampaignRoute->Map == 20 && &Current == SourceWorld.Get() && bWalking && PreviousCampaignFrame > 0.0)
            {
                const double FrameElapsed = FrameTime - PreviousCampaignFrame;
                ++WalkFrameCount;
                LongestWalkFrame = FMath::Max(LongestWalkFrame, FrameElapsed);
                if (FrameElapsed > 0.1)
                {
                    ++WalkHitchCount;
                    UE_LOG(LogTemp, Display, TEXT("L20 movement hitch: %.1f ms at step %d, %s."),
                        FrameElapsed * 1000.0, NextPoint, *Player.GetActorLocation().ToCompactString());
                }
            }
            PreviousCampaignFrame = FrameTime;
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                if (HeldKey.GetValue() == EKeys::Q)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Released);
                }
                HeldKey.Reset();
            }
            const auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the campaign replay lost the player or travel state"));
            }
            if (bCampaignMovementSample && WalkFrameCount >= 1800)
            {
                return Finish(&Player);
            }
            const FWadQuestProgress& Quest = Travel->Quest;
            const bool bTown = Current.GetAuthGameMode<ANativeWadTownGameMode>() != nullptr;
            if (bTown && CampaignLaunch)
            {
                if ((!bWholeCampaign && !Player.HasRestoredWadCheckpoint()) || Quest.ResultQuest != CampaignLaunch->Quest - 1 || !Quest.bSucceeded)
                {
                    return Finish(&Player, TEXT("the next quest needs the previous successful Town Hall checkpoint"));
                }
                if (Quest.bShowResult)
                {
                    Press(Player, EKeys::Enter);
                }
                else if (bCampaignChain && CampaignLaunch->Quest >= 2 && CampaignLaunch->Quest <= 6 && !bCampaignBoughtSupplies)
                {
                    return TickTownServices(Current, Player, Quest, true);
                }
                else if (NextPoint == 0 && Quest.Selected == CampaignLaunch->Quest - 1)
                {
                    RecordTownCharacter(Player);
                    Press(Player, EKeys::Right);
                    ++NextPoint;
                }
                else if (NextPoint == 1 && Quest.Selected == CampaignLaunch->Quest)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::Q);
                    ++NextPoint;
                }
                else if (NextPoint == 2)
                {
                    bool bSaved = false;
                    if (CaptureTownFrame(TEXT("briefing"), bSaved) && bSaved)
                    {
                        Press(Player, EKeys::Enter);
                        ++NextPoint;
                    }
                }
                return true;
            }
            if (bTown)
            {
                if (Travel->ArrivingFromMap != 0)
                {
                    return true;
                }
                const auto* Reward = Algo::FindByPredicate(CampaignQuests,
                    [&](const FCampaignTurnIn& Entry) { return Entry.Quest == CampaignRoute->Quest; });
                if (!Reward || !bCampaignExitRecorded || Quest.Active != INDEX_NONE || Quest.ResultQuest != Reward->Quest
                    || !Quest.bSucceeded || !Quest.bShowResult || Quest.Gold - CampaignGoldBeforeExit != Reward->Gold
                    || Player.GetExperience() - ExperienceBeforeTravel != Reward->Experience || Player.GetItemCount(Reward->Item) != 0)
                {
                    return Finish(&Player, TEXT("the Town Hall return did not apply the source quest reward and item turn-in"));
                }
                bool bSaved = false;
                const bool bHeadless = FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
                if (bHeadless || CaptureTownFrame(TEXT("town-success"), bSaved))
                {
                    const FString Slot = FString::Printf(TEXT("NativeWadCampaignTown%dReview"), Reward->Quest);
                    if ((!bSaved && !bHeadless) || !Player.SaveWadCheckpoint(Slot))
                    {
                        return Finish(&Player, TEXT("could not save the successful campaign return"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified campaign quest %d turn-in: %d gold, %d experience."),
                        Reward->Quest, Reward->Gold, Reward->Experience);
                    if (bWholeCampaign && Reward->Quest < UE_ARRAY_COUNT(CampaignQuests))
                    {
                        CampaignLaunch = &CampaignQuests[Reward->Quest];
                        CampaignRoute = nullptr;
                        bCampaignBoughtSupplies = false;
                        BeginCampaignMap(Current, Player);
                        return true;
                    }
                    return Finish(&Player);
                }
                return true;
            }
            if (&Current != SourceWorld.Get())
            {
                const int32 Destination = CampaignLaunch ? CampaignLaunch->FirstMap : CampaignRoute->Destination;
                const int32 QuestId = CampaignLaunch ? CampaignLaunch->Quest : CampaignRoute->Quest;
                const auto* Arrival = Algo::FindByPredicate(CampaignRoutes,
                    [Destination](const FCampaignRoute& Entry) { return Entry.Map == Destination; });
                const FString Source = CampaignRoute ? FString::Printf(TEXT("/Game/Maps/L%d"), CampaignRoute->Map) : FString();
                if (!bCampaignArrivalVerified)
                {
                    if (!Arrival || Quest.Active != QuestId || !Current.GetMapName().EndsWith(FString::Printf(TEXT("L%d"), Destination))
                        || !FVector2D(Player.GetActorLocation()).Equals(Arrival->Spawn, 5.0)
                        || (CampaignLaunch && !MatchesTownCharacter(Player))
                        || (Destination == 32 && !Travel->JournalPages.Contains(603))
                        || (CampaignRoute && (Quest.ResumeMaps.FindRef(QuestId) != Destination || !Travel->VisitedLevels.Contains(Source))))
                    {
                        return Finish(&Player, TEXT("the campaign did not reach the authored spawn with its previous state"));
                    }
                    bCampaignArrivalVerified = true;
                }
                // Verify the handoff before settling, since an active potion can delay checkpointing past a recovery tick.
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    const FString Slot = FString::Printf(TEXT("NativeWadCampaignL%dReview"), Destination);
                    if (!Player.SaveWadCheckpoint(Slot))
                    {
                        return Finish(&Player, TEXT("could not save the authored arrival"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified campaign arrival at L%d."), Destination);
                    if (bCampaignChain && !Arrival->Actions.IsEmpty())
                    {
                        CampaignRoute = Arrival;
                        CampaignLaunch = nullptr;
                        BeginCampaignMap(Current, Player);
                        bCampaignArrivedInRun = true;
                        bCampaignExitRecorded = false;
                        bCampaignActionStarted = false;
                        CampaignPickupCount = INDEX_NONE;
                        CampaignStepTime = -1.0;
                        return true;
                    }
                    return Finish(&Player);
                }
                return true;
            }
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || !CampaignRoute || Quest.Active != CampaignRoute->Quest
                || (!bCampaignArrivedInRun && !Player.HasRestoredWadCheckpoint()))
            {
                return Finish(&Player, TEXT("the campaign route needs its native map and matching checkpoint"));
            }
            if (NextPoint == 0 && CampaignRoute->Map == 20 && !bCampaignArrivedInRun
                && FCString::Stricmp(Current.URL.GetOption(TEXT("WadLoad="), TEXT("")), Campaign20ReturnSlot) == 0)
            {
                const FCampaignAction* Saved = Algo::FindByPredicate(CampaignRoute->Actions,
                    [](const FCampaignAction& Action) { return Action.Action == ECampaignAction::Checkpoint; });
                FVector2D Gate;
                if (!Saved || !(*Level)->GetSectorSurfaceHeights(18, Gate) || Gate.Y != 112.0
                    || !FVector2D(Player.GetActorLocation()).Equals(FVector2D(0, 2360), 12.0))
                {
                    return Finish(&Player, TEXT("the L20 return checkpoint needs the sheltered switch room and reopened gate"));
                }
                NextPoint = static_cast<int32>(Saved - CampaignRoute->Actions.GetData()) + 1;
            }
            if (NextPoint == 0 && CampaignRoute->Map == 34 && !bCampaignArrivedInRun
                && FCString::Stricmp(Current.URL.GetOption(TEXT("WadLoad="), TEXT("")), LighthouseReturnSlot) == 0)
            {
                const auto* Saved = Algo::FindByPredicate(CampaignRoute->Actions,
                    [](const FCampaignAction& Action) { return Action.Action == ECampaignAction::Checkpoint; });
                if (!Saved || !FVector2D(Player.GetActorLocation()).Equals({-521, -1222}, 16.0))
                {
                    return Finish(&Player, TEXT("the lighthouse return checkpoint requires the authored foothold"));
                }
                NextPoint = static_cast<int32>(Saved - CampaignRoute->Actions.GetData()) + 1;
            }
            if ((CampaignRoute->Map == 61 || CampaignRoute->Map == 40) && Player.GetStatusPower(NativeWadRules::LavaWalkStatus) == 0
                && Player.GetItemCount(813) > 0 && Level->FindLavaSurfaceZ(Player.GetActorLocation()).IsSet())
            {
                Stop(Player);
                if (Player.GetReadyItem() != 813 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 813); }
                else { Press(Player, EKeys::LeftControl); }
                CampaignStepTime = -1.0;
                return true;
            }
            if (CampaignRoute->Map == 71 || CampaignRoute->Map == 70)
            {
                if (Player.GetStatusPower(NativeWadRules::LavaWalkStatus) == 0 && Player.GetItemCount(813) > (CampaignRoute->Map == 71 ? 2 : 0)
                    && Level->FindLavaSurfaceZ(Player.GetActorLocation()).IsSet())
                {
                    Stop(Player);
                    if (Player.GetReadyItem() != 813 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 813); }
                    else { Press(Player, EKeys::LeftControl); }
                    CampaignStepTime = -1.0;
                    return true;
                }
                if (Player.GetStatusPower(NativeWadRules::InvisibleStatus) == 0 && Player.GetItemCount(361) > (CampaignRoute->Map == 71 ? 3 : 0))
                {
                    Stop(Player);
                    if (Player.GetReadyItem() != 361 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 361); }
                    else { Press(Player, EKeys::LeftControl); }
                    CampaignStepTime = -1.0;
                    return true;
                }
                if (!bCampaignConcealmentVerified && Player.IsConcealed() && !Player.GetVelocity().IsNearlyZero())
                {
                    int32 HiddenFrom = 0;
                    for (TActorIterator<ANativeWadGuardian> Enemy(&Current); Enemy; ++Enemy)
                    {
                        if (Enemy->GetSourceType() == 1014 && !Enemy->IsHidden() && !Enemy->IsDefeated())
                        {
                            if (Enemy->CanSeePlayer(Player)) { return Finish(&Player, TEXT("an archer could see the concealed campaign character")); }
                            ++HiddenFrom;
                        }
                    }
                    bCampaignConcealmentVerified = true;
                    UE_LOG(LogTemp, Display, TEXT("Verified the original invisibility effect conceals the moving campaign character from %d live L%d archers."), HiddenFrom, CampaignRoute->Map);
                }
            }
            if (CampaignRoute->Map == 72 && NextPoint > 2 && !Player.CanFly() && Player.GetItemCount(NativeWadRules::FlyingPotion) > 0)
            {
                Stop(Player);
                SetFlightThrust(Player, false);
                if (Player.GetReadyItem() != NativeWadRules::FlyingPotion || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, NativeWadRules::FlyingPotion);
                }
                else { Press(Player, EKeys::LeftControl); }
                CampaignStepTime = -1.0;
                return true;
            }
            TConstArrayView<FCampaignAction> Actions = CampaignRoute->Actions;
            if (NextPoint == 0 && CampaignRoute->Map == 73 && !bCampaignArrivedInRun)
            {
                const FString Slot(Current.URL.GetOption(TEXT("WadLoad="), TEXT("")));
                if (Slot == TEXT("NativeWadL73PotionsReview") || Slot == TEXT("NativeWadL73AllPotionsReview") || Slot == TEXT("NativeWadL73RecoveryReview"))
                {
                    const FCampaignAction* Saved = Algo::FindByPredicate(Actions, [&](const FCampaignAction& Action)
                    {
                        return Action.Action == ECampaignAction::Checkpoint && Slot == Action.Label;
                    });
                    if (!Saved || Player.GetItemCount(NativeWadRules::FlyingPotion) < (Slot == TEXT("NativeWadL73AllPotionsReview") ? 3 : Slot == TEXT("NativeWadL73PotionsReview") ? 2 : 1))
                    {
                        return Finish(&Player, TEXT("the L73 review checkpoint is missing its carried flight supplies"));
                    }
                    NextPoint = int32(Saved - Actions.GetData()) + 1;
                }
            }
            if (CampaignRoute->Map == 52 && FCString::Stricmp(Current.URL.GetOption(TEXT("WadLoad="), TEXT("")), TempleExitSlot) == 0)
            {
                Actions = Campaign52Exit;
            }
            if (!bWholeCampaign && CampaignRoute->Map == 44 && FParse::Param(FCommandLine::Get(), TEXT("WadReviewPlayerCombat")))
            {
                Actions = Campaign44Combat;
            }
            if (!Actions.IsValidIndex(NextPoint))
            {
                return Finish(&Player, TEXT("the original exit did not start travel"));
            }
            if (NextPoint == Actions.Num() - 1 && !bCampaignExitRecorded)
            {
                const FAAWadNativeLevelLink* Link = Level->GetLevelLinks().FindByPredicate([&](const auto& Entry)
                {
                    return Entry.SourceThing == CampaignRoute->ExitThing;
                });
                if (!Link)
                {
                    return Finish(&Player, TEXT("the map is missing its original exit Thing"));
                }
                bCampaignExitRecorded = true;
                CampaignGoldBeforeExit = Quest.Gold;
                ExperienceBeforeTravel = Player.GetExperience();
            }
            const FCampaignAction& Step = Actions[NextPoint];
            if (CampaignRoute->Map == 34 && Step.Action == ECampaignAction::Walk
                && FVector2D::DistSquared(FVector2D(Step.Target), FVector2D(-980, 1660)) < FMath::Square(200.0))
            {
                ANativeWadGuardian* Nearest = LighthouseTarget.Get();
                if (!Nearest || Nearest->IsDefeated() || !Nearest->CanSeePlayer(Player))
                {
                    Nearest = nullptr;
                    double Closest = FMath::Square(150.0);
                    for (TActorIterator<ANativeWadGuardian> Target(&Current); Target; ++Target)
                    {
                        const FVector Offset = Target->GetActorLocation() - Player.GetActorLocation();
                        if ((Target->GetSourceType() == 1003 || Target->GetEncounterName() == TEXT("Skeleton Knight"))
                            && !Target->IsDefeated() && !Target->IsHidden()
                            && FMath::Abs(Offset.Z) < 38.0 && Offset.SizeSquared2D() < Closest && Target->CanSeePlayer(Player))
                        {
                            Closest = Offset.SizeSquared2D();
                            Nearest = *Target;
                        }
                    }
                    LighthouseTarget = Nearest;
                }
                if (Nearest)
                {
                    FightCampaignEncounter(Player, *Nearest);
                    CampaignStepTime = -1.0;
                    return true;
                }
                if (bCombatBacking)
                {
                    SendKey(Player, EKeys::S, IE_Released);
                    bCombatBacking = false;
                }
                const FVector Position = Player.GetActorLocation();
                // A retreat below the tall boundary must return via the authored stair sectors.
                if (LighthouseStairPoint == INDEX_NONE && Position.Z < 60.0 && Position.X < -900.0
                    && FMath::IsWithinInclusive(Position.Y, 1350.0, 1380.0))
                {
                    LighthouseStairPoint = 0;
                }
                if (LighthouseStairPoint != INDEX_NONE)
                {
                    const FVector2D Stair[] = {{-940, 1120}, {-832, 1120}, {-832, 1476}};
                    if (WalkTo(Player, Stair[LighthouseStairPoint]) && ++LighthouseStairPoint == UE_ARRAY_COUNT(Stair))
                    {
                        LighthouseStairPoint = INDEX_NONE;
                    }
                    CampaignStepTime = -1.0;
                    return true;
                }
            }
            if (CampaignRoute->Map == 73)
            {
                ANativeWadGuardian* Finished = RouteCombatTarget.Get();
                if (Finished && Finished->GetWorld() == &Current && Finished->IsDefeated())
                {
                    RouteCombatTarget.Reset();
                    if (NextPoint > 0 && Actions[NextPoint - 1].Action == ECampaignAction::CrossDoor)
                    {
                        Stop(Player);
                        if (bCombatBacking) { SendKey(Player, EKeys::S, IE_Released); bCombatBacking = false; }
                        SetCombatStrafe(Player, 0);
                        Player.GetCharacterMovement()->StopMovementImmediately();
                        --NextPoint;
                        bCampaignActionStarted = false;
                        CampaignStepTime = -1.0;
                        UE_LOG(LogTemp, Display, TEXT("The L73 replay rejoins the last timed doorway after the close-range fight."));
                        return true;
                    }
                }
            }
            const int32 ThreatType = CampaignRoute->Map == 72 || CampaignRoute->Map == 73 ? 1011 : CampaignRoute->Map == 70 ? 1014
                : CampaignRoute->Map == 61 ? 1023 : CampaignRoute->Map == 153 ? 1007 : INDEX_NONE;
            if (ThreatType != INDEX_NONE && !((CampaignRoute->Map == 72 || CampaignRoute->Map == 73) && Player.CanFly())
                && !(CampaignRoute->Map == 70 && Player.GetStatusPower(NativeWadRules::InvisibleStatus) > 0)
                && (Step.Action == ECampaignAction::Walk || Step.Action == ECampaignAction::Use
                    || (CampaignRoute->Map == 70 && Step.Action == ECampaignAction::TouchSector)
                    || (CampaignRoute->Map == 61 && Step.Action == ECampaignAction::Jump)))
            {
                const FVector Location = Player.GetActorLocation();
                ANativeWadGuardian* Nearby = RouteCombatTarget.Get();
                if (!Nearby || Nearby->GetWorld() != &Current || Nearby->GetSourceType() != ThreatType
                    || Nearby->IsDefeated() || Nearby->IsHidden()
                    || FVector::DistSquared2D(Nearby->GetActorLocation(), Location) > FMath::Square(CampaignRoute->Map == 70 ? 600.0 : CampaignRoute->Map == 61 ? 400.0 : 200.0)
                    || FMath::Abs(Nearby->GetActorLocation().Z - Location.Z) >= (CampaignRoute->Map == 61 ? 120.0 : 45.0))
                {
                    Nearby = nullptr;
                    double Closest = FMath::Square(CampaignRoute->Map == 70 ? 450.0 : CampaignRoute->Map == 61 ? 250.0 : 100.0);
                    for (TActorIterator<ANativeWadGuardian> Target(&Current); Target; ++Target)
                    {
                        const FVector Offset = Target->GetActorLocation() - Location;
                        if (Target->GetSourceType() == ThreatType && !Target->IsDefeated() && !Target->IsHidden()
                            && Offset.SizeSquared2D() < Closest && FMath::Abs(Offset.Z) < (CampaignRoute->Map == 61 ? 120.0 : 45.0) && Target->CanSeePlayer(Player))
                        {
                            Nearby = *Target;
                            Closest = Offset.SizeSquared2D();
                        }
                    }
                }
                if (Nearby)
                {
                    if (RouteCombatTarget.Get() != Nearby)
                    {
                        RouteCombatTarget = Nearby;
                        RouteCombatStarted = Current.GetTimeSeconds();
                    }
                    if (Current.GetTimeSeconds() - RouteCombatStarted > 90.0)
                    {
                        return Finish(&Player, *FString::Printf(TEXT("L%d could not clear %s. Creature health %d, player health %d."),
                            CampaignRoute->Map, *Nearby->GetActorNameOrLabel(), Nearby->GetEncounterHealth(), Player.GetHealth()));
                    }
                    FightCampaignEncounter(Player, *Nearby);
                    CampaignStepTime = -1.0;
                    return true;
                }
                RouteCombatTarget.Reset();
                if (bCombatBacking)
                {
                    SendKey(Player, EKeys::S, IE_Released);
                    bCombatBacking = false;
                }
            }
            if (CampaignRoute->Map == 34 && Step.Action == ECampaignAction::OpenDoor
                && (Player.GetHealth() < 1450 || !Player.GetRunes().IsEmpty()))
            {
                CastCampaignRejuvenate(Player);
            }
            if (CampaignRoute->Map == 73 && Player.CanFly() && Step.Action != ECampaignAction::Walk
                && Step.Action != ECampaignAction::Fly && Step.Action != ECampaignAction::CrossDoor
                && Step.Action != ECampaignAction::FlightEnded)
            {
                FVector2D Limits;
                if (Level->GetSectorSurfaceHeights(Level->FindWadSector(FVector2D(Player.GetActorLocation())), Limits))
                {
                    const double Desired = Level->GetActorLocation().Z + FMath::Min(Limits.X + ANativeWadPlayer::CapsuleHalfHeight + 90.0,
                        Limits.Y - ANativeWadPlayer::CapsuleHalfHeight - 2.0);
                    SetFlightThrust(Player, Desired - Player.GetActorLocation().Z > 5.0);
                }
            }
            const double Now = Current.GetTimeSeconds();
            if (CampaignStepTime < 0.0)
            {
                CampaignStepTime = Now;
            }
            else if (Now - CampaignStepTime > (Step.Action == ECampaignAction::Rejuvenate || Step.Action == ECampaignAction::ManaAtLeast
                || Step.Action == ECampaignAction::FlightEnded || Step.Action == ECampaignAction::DefeatCreature
                || Step.Action == ECampaignAction::Defeat ? 240.0 : 30.0))
            {
                if (Step.Action == ECampaignAction::Pickup)
                {
                    UE_LOG(LogTemp, Display, TEXT("Campaign pickup type %d has inventory count %d, starting count %d. View %s, message %s."),
                        Step.Index, Player.GetItemCount(Step.Index), CampaignPickupCount,
                        *Player.GetPawnViewLocation().ToCompactString(), *Player.GetInteractionMessage());
                    int32 Reported = 0;
                    for (TActorIterator<ANativeWadPickup> Item(&Current); Item && Reported < 8; ++Item)
                    {
                        if (Item->GetItemType() == Step.Index
                            && FVector::DistSquared2D(Item->GetActorLocation(), Step.Target) < FMath::Square(256.0))
                        {
                            UE_LOG(LogTemp, Display, TEXT("Campaign pickup %s at %s, available %d."),
                                *Item->GetActorNameOrLabel(), *Item->GetActorLocation().ToCompactString(), Item->IsAvailable());
                            ++Reported;
                        }
                    }
                }
                FVector2D Heights;
                if ((Step.Action == ECampaignAction::Floor || Step.Action == ECampaignAction::Ceiling || Step.Action == ECampaignAction::CeilingAtMost
                    || Step.Action == ECampaignAction::OpenDoor) && (*Level)->GetSectorSurfaceHeights(Step.Index, Heights))
                {
                    UE_LOG(LogTemp, Error, TEXT("Campaign sector %d has floor Z%.6f and ceiling Z%.6f, expected %.6f for action %d."),
                        Step.Index, Heights.X, Heights.Y, Step.Target.Z, int32(Step.Action));
                }
                if (Step.Action == ECampaignAction::UseLockedDoor)
                {
                    UE_LOG(LogTemp, Error, TEXT("Campaign door interaction said: %s."), *Player.GetInteractionMessage());
                }
                if (Step.Action == ECampaignAction::Walk || Step.Action == ECampaignAction::Jump || Step.Action == ECampaignAction::Fly)
                {
                    const UCapsuleComponent* Capsule = Player.GetCapsuleComponent();
                    const FVector Start = Player.GetActorLocation();
                    const FVector End(Step.Target.X, Step.Target.Y, Start.Z);
                    FCollisionQueryParams Trace(SCENE_QUERY_STAT(CampaignWalkFailure), false, &Player);
                    Trace.bFindInitialOverlaps = false;
                    FHitResult Hit;
                    Current.SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Pawn,
                        FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight()), Trace);
                    UE_LOG(LogTemp, Display, TEXT("Campaign movement at Z%.2f hit %s on %s at %s. Jump peak Z%.2f."),
                        Start.Z, *GetNameSafe(Hit.GetComponent()), *GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToCompactString(), CampaignJumpPeak);
                }
                return Finish(&Player, *FString::Printf(TEXT("L%d step %d timed out waiting for action %d toward %s"), CampaignRoute->Map,
                    NextPoint, int32(Step.Action), *Step.Target.ToCompactString()));
            }
            if (TickCampaignAction(**Level, Player, Step))
            {
                if (CampaignRoute->Map == 20 && NextPoint == 1 && Step.Action == ECampaignAction::ManaAtLeast
                    && !Player.SaveWadCheckpoint(TEXT("NativeWadCampaignL20RestedReview")))
                {
                    return Finish(&Player, TEXT("the recovered character could not save at the L20 entrance"));
                }
                if (Step.Action == ECampaignAction::Defeat)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified %s drops the authored quest item %d on defeat."), Step.Label, Step.Index);
                }
                else if (Step.Label && Step.Action != ECampaignAction::Frame && Step.Action != ECampaignAction::Checkpoint)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified %s."), Step.Label);
                }
                ++NextPoint;
                bCampaignActionStarted = false;
                CampaignPickupCount = INDEX_NONE;
                CampaignStepTime = -1.0;
            }
            return true;
        }

        bool TickTemple(UWorld& Current, ANativeWadPlayer& Player)
        {
            const auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the Temple of Astarte review lost the player or campaign"));
            }
            const FWadQuestProgress& Quest = Travel->Quest;
            if (Current.GetAuthGameMode<ANativeWadTownGameMode>())
            {
                if (TempleReview != ETempleReview::Launch || !Player.HasRestoredWadCheckpoint()
                    || !Player.HasCompletedWadLevel(TEXT("L12")))
                {
                    return Finish(&Player, TEXT("the Temple launch needs the completed Exiguus checkpoint"));
                }
                if (Quest.bShowResult)
                {
                    Press(Player, EKeys::Enter);
                }
                else if (NextPoint == 0 && Quest.Selected == 3)
                {
                    RecordTownCharacter(Player);
                    Press(Player, EKeys::Right);
                    ++NextPoint;
                }
                else if (NextPoint == 1 && Quest.Selected == 4)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::Q);
                    ++NextPoint;
                }
                else if (NextPoint == 2)
                {
                    bool bSaved = false;
                    if (CaptureTownFrame(TEXT("briefing"), bSaved))
                    {
                        if (!bSaved)
                        {
                            return Finish(&Player, TEXT("could not capture the Temple of Astarte briefing"));
                        }
                        Press(Player, EKeys::Enter);
                        ++NextPoint;
                    }
                }
                return true;
            }
            if (Quest.Active != 4)
            {
                return Finish(&Player, TEXT("the Temple route lost the active quest"));
            }
            if (&Current != SourceWorld.Get())
            {
                const bool bLaunch = TempleReview == ETempleReview::Launch;
                const FVector2D Start = bLaunch ? FVector2D(-160, -189) : FVector2D(760, 3540);
                const FWadLevelCheckpoint* L51 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L51"));
                if (!Current.GetMapName().EndsWith(bLaunch ? TEXT("L51") : TEXT("L52"))
                    || !FVector2D(Player.GetActorLocation()).Equals(Start, 4)
                    || (bLaunch && !MatchesTownCharacter(Player))
                    || (!bLaunch && (!L51 || L51->SectorHeights.FindRef(115).Y != 168 || Quest.ResumeMaps.FindRef(4) != 52)))
                {
                    return Finish(&Player, TEXT("the Temple route did not reach the authored spawn or carry the gate state"));
                }
                if (Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    if (!Player.SaveWadCheckpoint(bLaunch ? TempleL51Slot : TempleL52Slot))
                    {
                        return Finish(&Player, TEXT("could not save the Temple arrival"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified Temple of Astarte arrival at %s."), bLaunch ? TEXT("L51") : TEXT("L52"));
                    return Finish(&Player);
                }
                return true;
            }
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || !Player.HasRestoredWadCheckpoint())
            {
                return Finish(&Player, TEXT("the Temple review needs its native map and checkpoint"));
            }
            if (TempleReview == ETempleReview::Reload)
            {
                FVector2D Gate;
                if (!Level->GetSectorSurfaceHeights(94, Gate) || Gate.Y != 0
                    || !FVector2D(Player.GetActorLocation()).Equals({-3402, 1440}, 4)
                    || Quest.ResumeMaps.FindRef(4) != 52 || !Travel->VisitedLevels.Contains(TEXT("/Game/Maps/L51"))
                    || Level->FindWadSector({-1706, 3488}) != 154
                    || Level->FindWadSector({-2400, 3712.5}) != INDEX_NONE)
                {
                    return Finish(&Player, TEXT("the L52 checkpoint lost the repaired courtyard, reset gate, exit approach, or campaign state"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the L52 exit checkpoint reloads with the quest active, gate reset, and courtyard boundary repaired."));
                return Finish(&Player);
            }
            return TempleReview == ETempleReview::L51 ? TickTempleL51(**Level, Player) : TickTempleL52(**Level, Player);
        }

        bool TickElmoreFinal(UWorld& Current, ANativeWadPlayer& Player, const UNativeWadTravelState& Travel)
        {
            if (Current.GetAuthGameMode<ANativeWadTownGameMode>())
            {
                return TickElmoreOrbTown(Player, Travel);
            }
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || Player.GetHealth() <= 0 || Travel.Quest.Active != 2
                || !Travel.JournalPages.Contains(423) || !Travel.JournalPages.Contains(424))
            {
                return Finish(&Player, TEXT("the Elmore finale lost the player, quest or journal"));
            }
            const FString Map = Current.GetMapName();
            const bool bArrivedL152 = L25Review == EL25Review::ElmoreL151Exit && Map.EndsWith(TEXT("L152"));
            const bool bArrivedL153 = L25Review == EL25Review::ElmoreL152 && Map.EndsWith(TEXT("L153"));
            if (bArrivedL152 || bArrivedL153)
            {
                const FVector2D Spawn = bArrivedL152 ? FVector2D(1484, 2315) : FVector2D(1596, 2544);
                bool bSaved = false;
                if (!FVector2D(Player.GetActorLocation()).Equals(Spawn, 4) || Travel.Quest.ResumeMaps.FindRef(2) != (bArrivedL152 ? 152 : 153))
                {
                    return Finish(&Player, TEXT("the Elmore handoff lost its authored spawn or active quest"));
                }
                if (Player.GetItemCount(NativeWadRules::ElmoreKey5) != 1
                    || (bArrivedL153 && Player.GetItemCount(NativeWadRules::ElmoreKey6) != 1))
                {
                    return Finish(&Player, TEXT("the Elmore handoff lost a key needed by the L153 doors"));
                }
                if (bArrivedL153)
                {
                    const auto* SavedL152 = Travel.VisitedLevels.Find(TEXT("/Game/Maps/L152"));
                    if (!SavedL152 || SavedL152->SectorHeights.FindRef(27).X != -476 || !SavedL152->ScriptLockedDoors.Contains(70))
                    {
                        return Finish(&Player, TEXT("the L153 handoff did not archive L152's reservoir and locked door"));
                    }
                }
                if (CaptureTownFrame(bArrivedL152 ? TEXT("l152-arrival") : TEXT("l153-arrival"), bSaved))
                {
                    if (!bSaved || !Player.SaveWadCheckpoint(bArrivedL152 ? ElmoreL152Slot : ElmoreL153Slot))
                    {
                        return Finish(&Player, TEXT("could not save the original Elmore map handoff"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("%s"), bArrivedL152 ? TEXT("Verified L151 Thing 84 takes the active quest to L152.")
                        : TEXT("Verified L152 Thing 266 takes the active quest and reservoir state to L153."));
                    return Finish(&Player);
                }
                return true;
            }
            switch (L25Review)
            {
            case EL25Review::ElmoreL151Exit: return TickElmoreMountainExit(**Level, Player);
            case EL25Review::ElmoreL152: return TickElmoreReservoir(**Level, Player);
            case EL25Review::ElmoreL153Puzzles: return TickElmoreVault(**Level, Player);
            default: return TickElmoreOrb(**Level, Player, Travel);
            }
        }

        bool TickL25(UWorld& Current, ANativeWadPlayer& Player)
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
            if (L25Review >= EL25Review::ElmoreL151Exit)
            {
                const auto* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
                return Travel ? TickElmoreFinal(Current, Player, *Travel)
                    : Finish(&Player, TEXT("the Elmore finale lost its campaign state"));
            }
            if (L25Review == EL25Review::ElmoreCombat64 || L25Review == EL25Review::ElmoreCombat65)
            {
                const TActorIterator<ANativeWadLevel> Level(&Current);
                return Level ? TickElmoreCombat(**Level, Player, L25Review == EL25Review::ElmoreCombat65)
                    : Finish(&Player, TEXT("could not load the native barbarian encounter map"));
            }
            if (L25Review >= EL25Review::ElmoreLaunch)
            {
                const auto* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
                if (!Travel || (L25Review != EL25Review::ElmoreLaunch && (Travel->Quest.Active != 2
                    || !Travel->JournalPages.Contains(423) || !Travel->JournalPages.Contains(424))))
                {
                    return Finish(&Player, TEXT("Elmore's Retreat lost its campaign progress or journal pages"));
                }
                if (L25Review == EL25Review::ElmoreLaunch)
                {
                    return TickElmoreLaunch(Player, *Travel, Current.GetAuthGameMode<ANativeWadTownGameMode>() != nullptr);
                }
                if (L25Review >= EL25Review::ElmoreL65Exit)
                {
                    const TActorIterator<ANativeWadLevel> Level(&Current);
                    return Level && Player.GetHealth() > 0 ? TickElmoreTravel(**Level, Player, *Travel)
                        : Finish(&Player, TEXT("the Elmore replay lost its player or native level"));
                }
                if (L25Review == EL25Review::ElmoreL64Reload && Current.GetMapName().EndsWith(TEXT("L65")))
                {
                    const auto* L64 = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L64"));
                    bool bSaved = false;
                    if (!FVector2D(Player.GetActorLocation()).Equals({-852, 2282}, 3) || !L64
                        || L64->ActivityWallTextures.FindRef(2685) != TEXT("SW2BRN2") || Travel->Quest.ResumeMaps.FindRef(2) != 65)
                    {
                        return Finish(&Player, TEXT("L64's authored exit lost its source state or the L65 spawn"));
                    }
                    if (CaptureTownFrame(TEXT("l65-arrival"), bSaved))
                    {
                        if (!bSaved || !Player.SaveWadCheckpoint(ElmoreL65Slot))
                        {
                            return Finish(&Player, TEXT("could not save the original L65 arrival"));
                        }
                        UE_LOG(LogTemp, Display, TEXT("Verified L64 Thing 227 travels to L65 and archives the solved lift and active quest."));
                        return Finish(&Player);
                    }
                    return true;
                }
            }
            if ((L25Review == EL25Review::L60Return || L25Review == EL25Review::L60Empty)
                && Current.GetAuthGameMode<ANativeWadTownGameMode>())
            {
                return TickAndrewTown(Player);
            }
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || Player.GetHealth() <= 0
                || (RiverProgressTime > 0.0 && FPlatformTime::Seconds() - RiverProgressTime > 25.0))
            {
                return Finish(&Player, TEXT("the L25 review lost its level or stopped making progress"));
            }
            switch (L25Review)
            {
            case EL25Review::Passages: return TickL25Passages(Current, **Level, Player);
            case EL25Review::Knights: return TickChurchKnights(Current, **Level, Player);
            case EL25Review::Exit: return TickChurchExit(Current, **Level, Player);
            case EL25Review::L20Reload: return TickL20Reload(Current, **Level, Player);
            case EL25Review::Scenery:
            case EL25Review::L20Scenery: return TickChurchScenery(Current, **Level, Player);
            case EL25Review::L20Exit: return TickL20Exit(Current, **Level, Player);
            case EL25Review::L60Reload: return TickL60Reload(Current, **Level, Player);
            case EL25Review::L60Puzzles: return TickL60Puzzles(Current, **Level, Player);
            case EL25Review::L60Relic:
            case EL25Review::L60Return:
            case EL25Review::L60Empty: return TickL60Relic(**Level, Player);
            case EL25Review::ElmoreL64:
            case EL25Review::ElmoreL64Reload: return TickElmoreL64(**Level, Player);
            case EL25Review::ElmoreL65:
            case EL25Review::ElmoreL65Reload: return TickElmoreL65(**Level, Player);
            default: return TickL25Key(**Level, Player);
            }
        }


        bool TickTownServices(UWorld& Current, ANativeWadPlayer& Player, const FWadQuestProgress& Progress, bool bCampaignShop = false)
        {
            constexpr int32 Potion = 800;
            const auto* Content = NativeWadTown::FindContent(Current);
            APlayerController* Controller = Player.GetController<APlayerController>();
            ANativeWadTownHUD* Hud = Controller ? Controller->GetHUD<ANativeWadTownHUD>() : nullptr;
            if (!Content || !Hud || (!bCampaignShop && !Player.HasRestoredWadCheckpoint()))
            {
                return Finish(&Player, TEXT("town services need a restored Town Hall save"));
            }
            if (TownReview == ETownReview::ServicesReload)
            {
                if (Progress.GetWalletCopper() != 50 || Progress.BankCopper != 700 || Player.GetItemCount(Potion) != 1)
                {
                    return Finish(&Player, TEXT("the bank balance, purse, or bought item did not survive reload"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the bank balance, purse and bought potion after restart."));
                return Finish(&Player);
            }
            if (Progress.bShowResult)
            {
                Press(Player, EKeys::Enter);
                return true;
            }
            int32 Width = 0;
            int32 Height = 0;
            Controller->GetViewportSize(Width, Height);
            const double Scale = FMath::Min(Width / 320.0, Height / 200.0);
            const bool bHeadlessShop = bCampaignShop && FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
            if (Scale <= 0 && !bHeadlessShop) { return true; }
            const FVector2D Origin((Width - 320 * Scale) / 2, (Height - 200 * Scale) / 2);
            const auto Click = [&](double X, double Y) { Hud->ClickAction(Origin + FVector2D(X, Y) * Scale); };
            const auto StockItem = [&](int32 Item)
            {
                int32 Index = 0;
                for (const auto& [Type, Tier] : Content->StoreStock)
                {
                    Index += Type < Item && Tier <= Progress.ShopTier;
                }
                for (int32 Page = 0; Page < Index / 42; ++Page) { Click(78, 129); }
                Index %= 42;
                return FVector2D(18 + Index % 6 * 17, 32 + Index / 6 * 14);
            };
            const auto ClickStock = [&](int32 Item)
            {
                const FVector2D Point = StockItem(Item);
                Click(Point.X, Point.Y);
            };
            const auto CloseService = [&]() { Click(200, 9); };
            const auto StowItem = [&]() { Click(220, 22); };
            if (TownReview == ETownReview::ServicesLoadout || bCampaignShop)
            {
                const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
                const int32 Melee = Player.CanUseWadItem(IronLongSword) ? IronLongSword
                    : Player.CanUseWadItem(NativeWadRules::IronMace) ? Progress.ShopTier >= 4 ? 28873 : NativeWadRules::IronMace : 202;
                const bool bBuyMelee = Player.GetItemCount(Melee) == 0;
                const int64 Before = Progress.GetWalletCopper();
                const auto ItemPrice = [&](int32 Type)
                {
                    const FWadInventoryDetails* Item = Catalog ? Catalog->ItemDetails.Find(Type) : nullptr;
                    return Item ? Item->ValueCopper : 0;
                };
                const bool bBeforeElmore = bCampaignShop && Progress.ResultQuest == 1;
                const bool bBeforeDestruction = bCampaignShop && Progress.ResultQuest == 5;
                const int32 AttackScrolls = bBeforeDestruction ? FMath::Max(0, 3 - Player.GetItemCount(370)) : 0;
                const int32 InvisibleScrolls = bBeforeDestruction ? FMath::Max(0, 6 - Player.GetItemCount(361)) : 0;
                const int32 LavaPotions = bBeforeDestruction ? FMath::Max(0, 3 - Player.GetItemCount(813)) : 0;
                const bool bBuyMana = bBeforeDestruction && Player.GetItemCount(362) == 0;
                const int32 WeaponPrice = (bBuyMelee ? ItemPrice(Melee) : 0) + AttackScrolls * ItemPrice(370) + InvisibleScrolls * ItemPrice(361) + LavaPotions * ItemPrice(813) + (bBuyMana ? ItemPrice(362) : 0);
                const int32 PotionLimit = bBeforeElmore || Progress.ResultQuest == 2 ? 6 : 18;
                const int32 BuyCount = int32(FMath::Clamp<int64>((Before - WeaponPrice) / 300, 0, PotionLimit));
                const int32 Price = WeaponPrice + 300 * BuyCount;
                const int32 PotionsBefore = Player.GetItemCount(GreaterHealingPotion);
                if ((Progress.ResultQuest < 2 && !bBeforeElmore) || !Progress.bSucceeded
                    || (!bCampaignShop && BuyCount < 6) || Before < Price)
                {
                    return Finish(&Player, TEXT("the provisioned combat run needs the previous reward and enough earned gold"));
                }
                const auto Buy = [&](int32 Type, int32 Count)
                {
                    if (bHeadlessShop)
                    {
                        for (int32 Index = 0; Index < Count; ++Index) { Player.TradeWadTownItem(Type, false, false); }
                    }
                    else
                    {
                        Click(122, 24);
                        const FVector2D Point = StockItem(Type);
                        for (int32 Index = 0; Index < Count; ++Index)
                        {
                            Click(Point.X, Point.Y);
                            StowItem();
                        }
                        CloseService();
                    }
                };
                if (bBuyMelee)
                {
                    Buy(Melee, 1);
                }
                if (AttackScrolls > 0)
                {
                    Buy(370, AttackScrolls);
                }
                if (InvisibleScrolls > 0) { Buy(361, InvisibleScrolls); }
                if (LavaPotions > 0) { Buy(813, LavaPotions); }
                if (bBuyMana)
                {
                    Buy(362, 1);
                }
                if (BuyCount > 0)
                {
                    Buy(GreaterHealingPotion, BuyCount);
                }
                if (Progress.GetWalletCopper() != Before - Price || Player.GetItemCount(Melee) == 0
                    || Player.GetItemCount(GreaterHealingPotion) != PotionsBefore + BuyCount
                    || (bBeforeDestruction && (Player.GetItemCount(370) < 3 || Player.GetItemCount(361) < 6 || Player.GetItemCount(813) < 3 || Player.GetItemCount(362) == 0))
                    || !Player.SaveWadCheckpoint(bBeforeElmore ? TEXT("NativeWadElmoreProvisionedTown")
                        : Progress.ResultQuest == 2 ? TEXT("NativeWadUndeadProvisionedTown") : TEXT("NativeWadProvisionedTownReview")))
                {
                    return Finish(&Player, TEXT("could not buy and save the permitted campaign equipment and healing supplies"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified earned-gold purchases of permitted weapon %d and %d healing potions."), Melee, BuyCount);
                if (bBeforeDestruction)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified the earned Destruction loadout includes three electric-attack scrolls, six invisibility scrolls (three reserved for L70), three Lava Walking potions (two reserved for L70), and mana recovery."));
                }
                if (bCampaignShop)
                {
                    bCampaignBoughtSupplies = true;
                    return true;
                }
                return Finish(&Player);
            }
            bool bSaved = false;
            switch (NextPoint)
            {
            case 0:
                if (Content->StoreStock.Num() != 187 || Content->BankStock.Num() != 21 || Progress.GetWalletCopper() != 1000
                    || Progress.ShopTier != 1 || Progress.BankCopper != 0 || Player.GetItemCount(Potion) != 0)
                {
                    return Finish(&Player, TEXT("the authored stock or opening Town Hall wallet is incorrect"));
                }
                Click(168, 24);
                Click(123, 36);
                Click(135, 46);
                if (Progress.GetWalletCopper() != 100 || Progress.BankCopper != 900)
                {
                    return Finish(&Player, *FString::Printf(TEXT("bank deposit or withdrawal produced purse %lld, bank %lld"),
                        Progress.GetWalletCopper(), Progress.BankCopper));
                }
                break;
            case 1:
                if (!CaptureTownFrame(TEXT("bank"), bSaved)) { return true; }
                if (!bSaved) { return Finish(&Player, TEXT("could not capture the Bank form")); }
                CloseService();
                Click(122, 24);
                ClickStock(Potion);
                if (Progress.GetWalletCopper() != 100 || Player.GetItemCount(Potion) != 0)
                {
                    return Finish(&Player, TEXT("the store allowed an unaffordable purchase"));
                }
                break;
            case 2:
                CloseService();
                Click(168, 24);
                Click(135, 46);
                Click(135, 46);
                CloseService();
                Click(122, 24);
                ClickStock(Potion);
                if (Progress.GetWalletCopper() != 150 || Progress.BankCopper != 700 || Player.GetItemCount(Potion) != 1)
                {
                    return Finish(&Player, TEXT("buying the 150-copper potion changed the wrong balance or inventory"));
                }
                if (Hud->ServiceBagItemCenter(Player.GetCarriedItems().Find(Potion)).X >= 0)
                {
                    return Finish(&Player, TEXT("the purchase skipped the mouse hand"));
                }
                StowItem();
                break;
            case 3:
            {
                if (!CaptureTownFrame(TEXT("store"), bSaved)) { return true; }
                if (!bSaved) { return Finish(&Player, TEXT("could not capture the Store form")); }
                const int32 Slot = Player.GetCarriedItems().Find(Potion);
                Click(281, 132);
                if (Hud->ServiceBagItemCenter(Slot).X >= 0)
                {
                    return Finish(&Player, TEXT("the inventory next-page arrow did not leave page one"));
                }
                Click(271, 132);
                Click(217, 143);
                if (Hud->ServiceBagItemCenter(Slot).X >= 0)
                {
                    return Finish(&Player, TEXT("the equipment button did not open the equipment banner"));
                }
                Click(217, 143);
                Click(308, 8);
                if (Hud->ServiceBagItemCenter(Slot).X >= 0)
                {
                    return Finish(&Player, TEXT("the inventory close button did not close the side banner"));
                }
                Click(13, 161);
                const FVector2D Sale = Hud->ServiceBagItemCenter(Slot);
                if (Sale.X < 213 || Sale.Y < 16)
                {
                    return Finish(&Player, TEXT("the stored potion is missing from the classic inventory grid"));
                }
                Click(Sale.X, Sale.Y);
                Click(18, 32);
                if (Progress.GetWalletCopper() != 200 || Player.GetItemCount(Potion) != 0)
                {
                    return Finish(&Player, TEXT("selling the potion did not return its original 50-copper resale value"));
                }
                CloseService();
                Click(122, 24);
                ClickStock(Potion);
                CloseService();
                if (Progress.GetWalletCopper() != 50 || !Player.SaveWadCheckpoint(TEXT("NativeWadTownServicesReview")))
                {
                    return Finish(&Player, TEXT("could not save the bank and store result"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the classic inventory grid, mouse hand, page arrows, equipment toggle, and close buttons."));
                UE_LOG(LogTemp, Display, TEXT("Verified Town Hall deposits, withdrawals, affordability, buying and selling."));
                return Finish(&Player);
            }
            }
            ++NextPoint;
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
            if (TownReview == ETownReview::Scroll)
            {
                return bTown ? TickTownScroll(Player) : Finish(&Player, TEXT("town scrolled into a different map"));
            }
            const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel)
            {
                return Finish(&Player, TEXT("native town lost its campaign state"));
            }
            if (TownReview == ETownReview::Services || TownReview == ETownReview::ServicesReload || TownReview == ETownReview::ServicesLoadout)
            {
                return bTown ? TickTownServices(Current, Player, Travel->Quest) : Finish(&Player, TEXT("town services left Town Hall"));
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
            if (L25Review == EL25Review::Exit)
            {
                return BeginChurchExit(**Level, Player);
            }
            if (bFreshCampaign)
            {
                BeginCampaignMap(Current, Player);
                TownReview = ETownReview::None;
                bSlice = true;
                bCampaign = true;
                UE_LOG(LogTemp, Display, TEXT("Fresh campaign entered L30 from Town Hall with %d XP and %d carried items."),
                    Player.GetExperience(), Player.GetCarriedItems().Num());
                return true;
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
            if (bFreshCampaign && Type == NativeWadRules::IronDagger && !Player.CanUseWadItem(Type))
            {
                Type = NativeWadRules::IronMace;
            }
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
                    Press(Player, EKeys::I);
                }
                return false;
            }
            if (!Player.IsInventoryOpen())
            {
                if (!bReleasedKey)
                {
                    Press(Player, EKeys::I);
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
                if (bWholeCampaign && FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
                {
                    Player.ClickInventorySlot(Button);
                }
                else
                {
                    HUD->ClickHUD(HUD->InventorySlotCenter(Button));
                }
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
                SliceStartingBerries = Player.GetItemCount(Berry);
                FVector2D Gate;
                const FVector2D Spawn = bSecondMap ? FVector2D(176,3464) : FVector2D(1608,432);
                const bool bStartingSector = bSecondMap
                    ? Level->GetSectorSurfaceHeights(31, Gate) && Gate.X == -250.0
                    : Level->GetSectorSurfaceHeights(129, Gate) && Gate.Y == 0.0;
                if (!FVector2D(Player.GetActorLocation()).Equals(Spawn, 2.0) || !bStartingSector
                    || (!bFreshCampaign && Player.GetItemCount(Berry) != 0) || Player.GetCharacterLevel() != 1 || Player.GetMana() != Player.GetMaxMana())
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
                if (Player.GetItemCount(Type) == 1 + (Type == Berry ? SliceStartingBerries : 0))
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
                if (Player.GetItemCount(Berry) == SliceStartingBerries && FoodBeforeBerry != INDEX_NONE)
                {
                    const int32 RestoredFood = FMath::Min(MaxFood, FoodBeforeBerry + BerryFood);
                    // The next review tick can observe one scheduled hunger tick after the berry was used.
                    if (Player.GetFood() > RestoredFood || Player.GetFood() < RestoredFood - 1)
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
                UE_LOG(LogTemp, Display, TEXT("Verified disk checkpoint restored character stats, ready starter weapon, removed pickups, defeated Guardian, raised gate and exit switch lock."));
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
                    || Player.GetHealth() != HealthBeforeTravel || Player.GetEquippedWeapon() != (Player.CanUseWadItem(NativeWadRules::IronDagger) ? NativeWadRules::IronDagger : NativeWadRules::IronMace))
                {
                    return Finish(&Player, TEXT("the authored L301 link did not bring the same player to the L531 spawn"));
                }
                Camera.Reset();
                bWalking = false;
                Advance(ERiverStage::Arrival);
                UE_LOG(LogTemp, Display, TEXT("Verified L301 thing 304 travels to the original L531 spawn, with the same XP, health and ready starter weapon."));
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
                if (!XY.Equals(FVector2D(3440, 2944), 3.0) || Player.GetEquippedWeapon() != (Player.CanUseWadItem(NativeWadRules::IronDagger) ? NativeWadRules::IronDagger : NativeWadRules::IronMace))
                {
                    return Finish(&Player, TEXT("the river run requires the original L301 spawn with a ready starter weapon"));
                }
                Advance(ERiverStage::Encounter);
                break;
            case ERiverStage::Encounter:
            {
                if (bWholeCampaign && Player.GetMana() > NativeWadRules::RejuvenateCost
                    && (Player.GetHealth() < 1750 || !Player.GetRunes().IsEmpty()))
                {
                    CastCampaignRejuvenate(Player);
                    break;
                }
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
                if (bFreshCampaign)
                {
                    BeginCampaignMap(Current, Player);
                    bRiverReview = false;
                    bRavineReview = true;
                    return true;
                }
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
                        || Player.GetHealth() != HealthBeforeTravel || Player.GetEquippedWeapon() != (Player.CanUseWadItem(NativeWadRules::IronDagger) ? NativeWadRules::IronDagger : NativeWadRules::IronMace))
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
                if (!Position.Equals(Start, 3.0) || Player.GetEquippedWeapon() != (Player.CanUseWadItem(NativeWadRules::IronDagger) ? NativeWadRules::IronDagger : NativeWadRules::IronMace))
                {
                    return Finish(&Player, TEXT("the route requires the original player spawn with a ready starter weapon"));
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
                    || Player.GetEquippedWeapon() != (Player.CanUseWadItem(NativeWadRules::IronDagger) ? NativeWadRules::IronDagger : NativeWadRules::IronMace))
                {
                    return Finish(&Player, TEXT("the exit did not preserve the character at the original destination spawn"));
                }
                if (bFreshCampaign)
                {
                    BeginCampaignMap(Current, Player);
                    bRavineStarted = false;
                    Trail.Reset();
                    UE_LOG(LogTemp, Display, TEXT("Fresh campaign continued into %s with the same character."), Destination);
                    if (bL22Journey)
                    {
                        bRavineReview = false;
                        TownReview = ETownReview::Tour;
                    }
                    else
                    {
                        bL22Journey = true;
                    }
                    return true;
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

        bool TickEconomy(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr int32 Silver = 102;
            constexpr const TCHAR* Slot = TEXT("NativeWadEconomyWaterWalk");
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
            const FWadInventoryDetails* Ring = Catalog ? Catalog->ItemDetails.Find(WaterWalkRing) : nullptr;
            if (!Travel || !Ring || Ring->ValueCopper < CoinValueCopper(Silver) || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the economy review needs the authored ring and a living character"));
            }
            FWadQuestProgress& Progress = Travel->Quest;
            if (Current.GetMapName().EndsWith(TEXT("L30")))
            {
                const TActorIterator<ANativeWadLevel> Level(&Current);
                ANativeWadPickup* Coin = nullptr;
                for (TActorIterator<ANativeWadPickup> It(&Current); It; ++It)
                {
                    if (It->ActorHasTag(TEXT("WAD Item Thing 302"))) { Coin = *It; break; }
                }
                if (NextPoint == 0)
                {
                    if (!Level || !Coin || Coin->GetItemType() != Silver || !PlaceReviewPlayer(**Level, Player, {3344, 3258}))
                    {
                        return Finish(&Player, TEXT("could not reach L30's original silver coin"));
                    }
                    Progress.SetWalletCopper(Ring->ValueCopper - CoinValueCopper(Silver));
                    Aim(Player, Coin->GetActorLocation());
                    if (!Player.Use())
                    {
                        return Finish(&Player, TEXT("Use could not reach the authored silver coin"));
                    }
                    ++NextPoint;
                }
                if (Progress.GetWalletCopper() != Ring->ValueCopper || Player.GetItemCount(Silver) != 0)
                {
                    return Finish(&Player, TEXT("the silver pickup did not complete the purse balance"));
                }
                ++NextPoint;
                Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(NativeWadTown::ObjectPath)));
                return true;
            }
            if (Current.GetMapName().EndsWith(TEXT("WadTown")))
            {
                const auto* Snapshot = Travel->VisitedLevels.Find(TEXT("/Game/Maps/L30"));
                APlayerController* Controller = Player.GetController<APlayerController>();
                auto* Hud = Controller ? Controller->GetHUD<ANativeWadTownHUD>() : nullptr;
                if (!Hud || !Snapshot || Snapshot->RemainingPickups.Contains(TEXT("WAD Item Thing 302")))
                {
                    return Finish(&Player, TEXT("the coin was still present in the archived dungeon"));
                }
                int32 Width = 0, Height = 0;
                Controller->GetViewportSize(Width, Height);
                const double Scale = FMath::Min(Width / 320.0, Height / 200.0);
                if (Scale <= 0.0) { return true; }
                const FVector2D Origin((Width - 320 * Scale) * 0.5, (Height - 200 * Scale) * 0.5);
                const auto Click = [Hud, Origin, Scale](double X, double Y) { Hud->ClickAction(Origin + FVector2D(X, Y) * Scale); };
                Click(168, 24);
                Click(18, 88);
                Click(220, 22);
                Click(200, 9);
                if (Progress.GetWalletCopper() != 0 || Player.GetItemCount(WaterWalkRing) != 1)
                {
                    return Finish(&Player, TEXT("the Bank did not sell the ring for the exact purse balance"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the authored L30 coin paid the last %d copper of the %d-copper Bank ring."),
                    CoinValueCopper(Silver), Ring->ValueCopper);
                Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L301.L301"))));
                return true;
            }

            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || !Current.GetMapName().EndsWith(TEXT("L301")))
            {
                return Finish(&Player, TEXT("the economy review did not reach L301"));
            }
            constexpr double Surface = 32.0;
            const FVector Position = Player.GetActorLocation();
            const double Feet = Position.Z - ANativeWadPlayer::CapsuleHalfHeight;
            const bool bStanding = Player.GetCharacterMovement()->IsMovingOnGround();
            if (bEconomyReload)
            {
                if (NextPoint == 0)
                {
                    if (!Player.HasRestoredWadCheckpoint() || !bStanding) { return true; }
                    if (!Player.IsWaterWalking() || Player.GetReadyItem() != WaterWalkRing
                        || Progress.GetWalletCopper() != 0 || !FMath::IsNearlyEqual(Feet, Surface, 3.0))
                    {
                        return Finish(&Player, TEXT("the saved purse, ready ring, or water surface was not restored"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the Bank ring, purse, and standing on the water surface after process restart."));
                    ++NextPoint;
                    Press(Player, EKeys::I);
                    return true;
                }
                if (NextPoint == 1)
                {
                    if (!Player.IsInventoryOpen()) { Press(Player, EKeys::I); return true; }
                    Player.ClickInventorySlot(INDEX_NONE);
                    Press(Player, EKeys::I);
                    ++NextPoint;
                    return true;
                }
                if (Player.IsWaterWalking() || !bStanding || !FMath::IsNearlyEqual(Feet, Surface - 20.0, 3.0)) { return true; }
                UE_LOG(LogTemp, Display, TEXT("Verified unreadying the ring returns the character to the submerged floor."));
                return Finish(&Player);
            }
            if (NextPoint == 2)
            {
                if (!PlaceReviewPlayer(**Level, Player, {2256, 3168}))
                {
                    return Finish(&Player, TEXT("could not position the water walking comparison"));
                }
                ++NextPoint;
                return true;
            }
            if (NextPoint == 3)
            {
                if (!bStanding || !FMath::IsNearlyEqual(Feet, Surface - 20.0, 3.0)) { return true; }
                if (Level->GetCurrentInput(Position - FVector(0, 0, ANativeWadPlayer::CapsuleHalfHeight)).IsNearlyZero())
                {
                    return Finish(&Player, TEXT("the submerged comparison did not have its original current"));
                }
                ++NextPoint;
            }
            if (Player.GetReadyItem() != WaterWalkRing || Player.IsInventoryOpen())
            {
                ReadyCampaignItem(Player, WaterWalkRing);
                return true;
            }
            if (!bStanding || !Player.IsWaterWalking() || !FMath::IsNearlyEqual(Feet, Surface, 3.0)) { return true; }
            if (!Level->GetCurrentInput(Position - FVector(0, 0, ANativeWadPlayer::CapsuleHalfHeight), true).IsNearlyZero()
                || !Player.SaveWadCheckpoint(Slot))
            {
                return Finish(&Player, TEXT("the ring failed to suppress the river current or save on its surface"));
            }
            UE_LOG(LogTemp, Display, TEXT("Verified the ring raises the player from the submerged L301 floor to the surface and suppresses the current."));
            return Finish(&Player);
        }

        bool TickMusicTravel(UWorld& Current, ANativeWadPlayer& Player)
        {
            const auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel || !Travel->IsBackgroundMusicPlaying())
            {
                if (NextPoint == 3)
                {
                    return Finish(&Player, TEXT("L61's music stopped during the loop check"));
                }
                return true;
            }
            if (NextPoint == 0)
            {
                if (Travel->BackgroundMusicName() != TEXT("MUSIC2"))
                {
                    return Finish(&Player, TEXT("L30 selected the wrong authored track"));
                }
                ++NextPoint;
                Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L301.L301"))));
            }
            else if (NextPoint == 1 && Current.GetMapName().EndsWith(TEXT("L301")) && Travel->BackgroundMusicName() == TEXT("MUSIC3"))
            {
                ++NextPoint;
                Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L61.L61"))));
            }
            else if (NextPoint == 2 && Current.GetMapName().EndsWith(TEXT("L61")) && Travel->BackgroundMusicName() == TEXT("L1"))
            {
                const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
                const USoundWave* Track = Catalog ? Catalog->MusicByMap.FindRef(TEXT("L61")).Get() : nullptr;
                if (!Track || !Track->bLooping || Track->Duration <= 0.0f)
                {
                    return Finish(&Player, TEXT("L61's authored music is missing a looping sound wave"));
                }
                StageTime = Current.GetTimeSeconds() + Track->Duration + 1.0;
                ++NextPoint;
            }
            else if (NextPoint == 3 && Current.GetTimeSeconds() >= StageTime)
            {
                UE_LOG(LogTemp, Display, TEXT("Verified music switched from MUSIC2 in L30 to MUSIC3 in L301 and continued past the first L1 loop in L61."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickLavaTerrain(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
            if (!Level) { return Finish(&Player, TEXT("the lava terrain check needs L71")); }
            const bool bReload = Player.GetWorld()->URL.HasOption(TEXT("WadLoad"));
            if (bReload && !Player.HasRestoredWadCheckpoint()) { return true; }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            if (NextPoint == 0)
            {
                FWadLevelCheckpoint State;
                if (!Level->WriteCheckpoint(State, true)) { return Finish(&Player, TEXT("could not prepare the controlled lava terrain fixture")); }
                for (auto& [Id, Generator] : State.CreatureGenerators) { Generator.bPaused = true; }
                for (auto& [Name, Encounter] : State.Encounters) { Encounter.Health = 0; }
                if (!Level->RestoreCheckpoint(State)) { return Finish(&Player, TEXT("could not isolate the terrain check from encounters")); }
                if (bReload)
                {
                    if (Player.GetItemCount(813) != 0 || Player.GetStatusPower(LavaWalkStatus) == 0)
                    {
                        return Finish(&Player, TEXT("Lava Walking or its consumed potion failed to reload"));
                    }
                    NextPoint = 5;
                    StageTime = Now + 2.0;
                    HealthBeforeCast = Player.GetHealth();
                }
                else
                {
                    if (Player.GetCapsuleComponent()->GetCollisionResponseToChannel(ANativeWadLevel::LavaSurfaceChannel) != ECR_Ignore
                        || !PlaceReviewPlayer(**Level, Player, {-240, -652}) || !Level->FindLavaSurfaceZ(Player.GetActorLocation()))
                    {
                        return Finish(&Player, TEXT("the default character did not ignore the original lava surface above its walking floor"));
                    }
                    Player.CollectItem(813);
                    HealthBeforeCast = Player.GetHealth();
                    StageTime = Now + 2.0;
                    NextPoint = 1;
                }
                return true;
            }
            switch (NextPoint)
            {
            case 1:
                if (Now < StageTime) { break; }
                if (Player.GetHealth() > HealthBeforeCast - 400)
                {
                    return Finish(&Player, TEXT("unprotected standing in the original lava did not deal damage"));
                }
                ++NextPoint;
                break;
            case 2:
                if (Player.GetReadyItem() != 813 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 813); break; }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 3:
                if (Player.GetItemCount(813) != 0 || Player.GetStatusPower(LavaWalkStatus) == 0)
                {
                    return Finish(&Player, TEXT("the regular ready-hand action did not apply and consume Lava Walking"));
                }
                StageTime = Now + 2.0;
                HealthBeforeCast = Player.GetHealth();
                ++NextPoint;
                break;
            case 4:
            case 5:
                if (Now < StageTime) { break; }
                {
                    const TOptional<double> Surface = Level->FindLavaSurfaceZ(Player.GetActorLocation());
                    const double Feet = Player.GetActorLocation().Z - Player.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                    if (!Surface || Feet < Surface.GetValue() - 0.5 || Player.GetHealth() < HealthBeforeCast
                        || Player.GetCapsuleComponent()->GetCollisionResponseToChannel(ANativeWadLevel::LavaSurfaceChannel) != ECR_Block)
                    {
                        return Finish(&Player, TEXT("Lava Walking did not stand on the original surface without damage"));
                    }
                }
                if (NextPoint == 4)
                {
                    if (!Player.SaveWadCheckpoint(TEXT("NativeWadLavaTerrainReview"))) { return Finish(&Player, TEXT("could not save Lava Walking on the actual surface")); }
                    UE_LOG(LogTemp, Display, TEXT("Verified default lava collision, unprotected terrain damage, consumed Lava Walking potion, surface standing, and protection on original L71 terrain."));
                    return Finish(&Player);
                }
                ++NextPoint;
                break;
            case 6:
                if (Player.GetStatusPower(LavaWalkStatus) != 0) { break; }
                if (Player.GetCapsuleComponent()->GetCollisionResponseToChannel(ANativeWadLevel::LavaSurfaceChannel) != ECR_Ignore)
                {
                    return Finish(&Player, TEXT("lava-surface collision remained enabled after the effect expired"));
                }
                HealthBeforeCast = Player.GetHealth();
                StageTime = Now + 2.0;
                ++NextPoint;
                break;
            case 7:
                if (Now < StageTime) { break; }
                if (Player.GetHealth() > HealthBeforeCast - 200) { return Finish(&Player, TEXT("lava damage did not resume after protection expired")); }
                UE_LOG(LogTemp, Display, TEXT("Verified Lava Walking after restart and resumed falling and terrain damage after expiration."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickInfiniteGenerator(ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
            if (!Level) { return Finish(&Player, TEXT("the unlimited generator check needs L71")); }
            const bool bReload = Player.GetWorld()->URL.HasOption(TEXT("WadLoad"));
            if (bReload && !Player.HasRestoredWadCheckpoint()) { return true; }
            FWadLevelCheckpoint Snapshot;
            if (!Level->WriteCheckpoint(Snapshot, true) || !Snapshot.CreatureGenerators.Contains(0))
            {
                return Finish(&Player, TEXT("L71 must import its original unlimited generator"));
            }
            const auto CountLive = [&]()
            {
                int32 Count = 0;
                for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                {
                    Count += It->GetSourceType() == 1014 && !It->IsHidden() && !It->IsDefeated();
                }
                return Count;
            };
            const auto ClearLive = [&]()
            {
                for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                {
                    if (It->GetSourceType() == 1014 && !It->IsHidden()) { It->RestoreEncounterHealth(0); }
                }
            };
            const double Now = Player.GetWorld()->GetTimeSeconds();
            if (NextPoint == 0 && bReload)
            {
                if (Snapshot.CreatureGenerators[0].Released != 41 || Snapshot.CreatureGenerators[0].Remaining <= 0 || CountLive() != 1)
                {
                    return Finish(&Player, TEXT("release 41, the live creature, or the running generator clock failed to reload"));
                }
                ClearLive();
                NextPoint = 3;
                StageTime = Now + 12.0;
            }
            else if (NextPoint == 0)
            {
                int32 Pool = 0;
                Snapshot.Encounters.Reset();
                for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                {
                    if (It->Tags.IsEmpty()) { continue; }
                    const bool bPool = It->Tags[0].ToString().StartsWith(TEXT("WAD Generator 0 Creature "));
                    const int32 Health = bPool && Pool++ < 15 ? 4000 : 0;
                    Snapshot.Encounters.Add(It->Tags[0], {It->GetActorTransform(), Health, 0, 5.0});
                }
                if (Pool != 40) { return Finish(&Player, TEXT("the unlimited generator fixture needs its forty reusable actors")); }
                for (auto& [Id, State] : Snapshot.CreatureGenerators)
                {
                    State.bPaused = Id != 0;
                }
                Snapshot.CreatureGenerators[0].Released = 40;
                Snapshot.CreatureGenerators[0].Remaining = 0.05f;
                if (!Level->RestoreCheckpoint(Snapshot)) { return Finish(&Player, TEXT("could not seed the controlled generator-cap fixture")); }
                for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                {
                    if (It->GetSourceType() == 1014)
                    {
                        It->SetActorTickEnabled(false);
                        It->GetCharacterMovement()->SetComponentTickEnabled(false);
                    }
                }
                NextPoint = 1;
                StageTime = Now + 6.0;
                return true;
            }
            const FWadCreatureGeneratorState& State = Snapshot.CreatureGenerators[0];
            if (NextPoint == 1 && Now >= StageTime)
            {
                if (State.Released != 40 || CountLive() != 15) { return Finish(&Player, TEXT("the unlimited generator overwrote a live creature or ignored its active cap")); }
                ClearLive();
                NextPoint = 2;
                StageTime = Now + 12.0;
                return true;
            }
            if ((NextPoint == 2 || NextPoint == 3) && State.Released >= (NextPoint == 2 ? 41 : 42))
            {
                if (CountLive() != 1 || State.Remaining <= 0)
                {
                    return Finish(&Player, TEXT("the unlimited generator did not create exactly one fresh live creature"));
                }
                for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                {
                    if (It->GetSourceType() == 1014 && !It->IsHidden() && !It->IsDefeated()
                        && (It->GetEncounterHealth() != 4000 || It->GetPoisonLevel() != 0
                            || !FVector2D(It->GetActorLocation()).Equals(FVector2D(-480, -952), 12.0)))
                    {
                        return Finish(&Player, TEXT("a recycled creature kept old health, poison, or its previous position"));
                    }
                }
                if (NextPoint == 2 && !Player.SaveWadCheckpoint(TEXT("NativeWadInfiniteGeneratorReview")))
                {
                    return Finish(&Player, TEXT("could not save the active unlimited generator after release 41"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the controlled L71 unlimited-generator fixture at release %d, with the active cap and fresh creature state. Restart=%d."), State.Released, bReload);
                return Finish(&Player);
            }
            if (Now > StageTime && NextPoint > 1) { return Finish(&Player, TEXT("the unlimited generator stopped beyond its reusable pool")); }
            return true;
        }

        bool TickElementalMelee(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const bool bReload = Player.GetWorld()->URL.HasOption(TEXT("WadLoad"));
            if (bReload && !Player.HasRestoredWadCheckpoint()) { return true; }
            if (bReload && NextPoint == 0)
            {
                if (Player.GetItemCount(370) != 0 || Player.GetReadyItem() != IronMace
                    || Player.GetMeleeDamageMask() != (NormalDamage | DamageFlag(EProjectileDamage::Electricity)))
                {
                    return Finish(&Player, TEXT("the consumed electric-attack scroll or its active effect failed to reload"));
                }
                NextPoint = 5;
            }
            if (NextPoint == 0)
            {
                const FTransform Transform(FRotator::ZeroRotator, Player.GetActorLocation() + FVector(55, 0, 0));
                ANativeWadGuardian* Target = Player.GetWorld()->SpawnActorDeferred<ANativeWadGuardian>(
                    ANativeWadGuardian::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
                if (!Target) { return Finish(&Player, TEXT("could not create the controlled resistance target")); }
                FWadEncounterProfile Profile;
                Profile.Name = TEXT("Controlled chain-armored resistance target");
                Profile.MaxHealth = 4000;
                Profile.MeleeDamage = Profile.BoltDamage = Profile.RegenPerUpdate = 0;
                Profile.WalkSpeed = 0;
                Profile.ArmorClass = 1;
                Profile.ResistanceMask = NormalDamage | DamageFlag(EProjectileDamage::Fire);
                Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
                UGameplayStatics::FinishSpawningActor(Target, Transform);
                ElementalTarget = Target;
                Player.CollectItem(IronMace);
                Player.CollectItem(370);
                ++NextPoint;
            }
            ANativeWadGuardian* Target = ElementalTarget.Get();
            switch (NextPoint)
            {
            case 1:
                if (Player.GetReadyItem() != IronMace || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, IronMace); break; }
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Player.GetWorld()->GetTimeSeconds() + 2.0;
                ++NextPoint;
                break;
            case 2:
                if (Player.GetWorld()->GetTimeSeconds() < StageTime) { break; }
                if (Target->GetEncounterHealth() != 4000) { return Finish(&Player, TEXT("normal melee bypassed the authored resistance")); }
                if (Player.GetReadyItem() != 370 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 370); break; }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 3:
                if (Player.GetItemCount(370) != 0 || Player.GetStatusPower(ElectricityAttackStatus) != 1)
                {
                    return Finish(&Player, TEXT("the original scroll did not consume and apply electric attack"));
                }
                if (Player.GetReadyItem() != IronMace || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, IronMace); break; }
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Player.GetWorld()->GetTimeSeconds() + 2.0;
                ++NextPoint;
                break;
            case 4:
                if (Player.GetWorld()->GetTimeSeconds() < StageTime) { break; }
                // Strength 15 * mace damage 10 / 2 = 75. Chain adds 25%, then one of two elements is resisted.
                if (Target->GetEncounterHealth() != 3953 || !Player.SaveWadCheckpoint(TEXT("NativeWadElementalMeleeReview")))
                {
                    return Finish(&Player, *FString::Printf(TEXT("the normal attack did not apply the expected 47 elemental damage. Target health=%d"), Target->GetEncounterHealth()));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified normal-damage immunity, electric-attack scroll consumption, and a regular mace hit for 47 damage through chain armor and one resisted element."));
                return Finish(&Player);
            case 5:
                if (Player.GetStatusPower(ElectricityAttackStatus) != 0) { return true; }
                if (Player.GetMeleeDamageMask() != NormalDamage) { return Finish(&Player, TEXT("electric attack persisted after the scroll expired")); }
                Player.CollectItem(234);
                ++NextPoint;
                break;
            case 6:
                if (Player.GetReadyItem() != 234 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 234); break; }
                if (Player.GetMeleeDamageMask() != (NormalDamage | DamageFlag(EProjectileDamage::Electricity)))
                {
                    return Finish(&Player, TEXT("the original electric mace lost its readied effect"));
                }
                ++NextPoint;
                break;
            case 7:
                if (Player.GetReadyItem() != IronMace || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, IronMace); break; }
                if (Player.GetMeleeDamageMask() != NormalDamage) { return Finish(&Player, TEXT("the electric mace effect remained after unreadying it")); }
                UE_LOG(LogTemp, Display, TEXT("Verified electric-attack persistence and expiration, and the original enchanted mace's ready/unready effects."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickPendingCharacter(ANativeWadPlayer& Player)
        {
            if (Player.GetWorld()->URL.HasOption(TEXT("WadLoad")) && !Player.HasRestoredWadCheckpoint()) { return true; }
            if (Player.HasRestoredWadCheckpoint())
            {
                if (!Player.CanChooseWadClass() || Player.GetItemCount(800) != 1
                    || !Player.ChooseWadClass(NativeWadRules::MageClass) || Player.CanChooseWadClass()
                    || Player.GetItemCount(800) != 1 || Player.GetReadyItem() != NativeWadRules::IronDagger)
                {
                    return Finish(&Player, TEXT("a pre-creation purchase prevented class choice after reload or was lost when the Mage kit was assigned"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the controlled carried item and pending class choice survive restart. Mage creation retains the purchased item and cannot be repeated."));
                return Finish(&Player);
            }
            if (!Player.CanChooseWadClass() || !Player.CollectItem(800)
                || Player.GetItemCount(800) != 1 || !Player.CanChooseWadClass()
                || !Player.SaveWadCheckpoint(TEXT("NativeWadPendingCharacterReview")))
            {
                return Finish(&Player, TEXT("could not save the controlled carried-item fixture before choosing a class"));
            }
            UE_LOG(LogTemp, Display, TEXT("Verified a controlled carried-item fixture can be saved before character creation."));
            return Finish(&Player);
        }

        bool TickClassEquipment(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr const TCHAR* Slot = TEXT("NativeWadClassEquipmentReview");
            constexpr const TCHAR* LegacySlot = TEXT("NativeWadClassLegacyReview");
            if (Player.GetWorld()->URL.HasOption(TEXT("WadLoad")) && !Player.HasRestoredWadCheckpoint())
            {
                return true;
            }
            if (Player.HasRestoredWadCheckpoint())
            {
                const bool bLegacy = FCString::Stricmp(Player.GetWorld()->URL.GetOption(TEXT("WadLoad="), TEXT("")), LegacySlot) == 0;
                const int32 Helmet = Player.GetCarriedItems().Find(704);
                if (Player.GetCharacterClass() != PriestClass || Player.CanChooseWadClass()
                    || Player.GetReadyItem() != (bLegacy ? 0 : IronMace)
                    || Player.GetItemCount(204) != 1 || Player.GetItemCount(Crossbow) != 1 || Player.GetItemCount(708) != 1
                    || Player.IsWornInventorySlot(Player.GetCarriedItems().Find(708))
                    || Player.IsWornInventorySlot(Helmet) == bLegacy)
                {
                    return Finish(&Player, TEXT("class equipment or a restricted legacy slot failed to reload"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified class equipment after restart. Restricted legacy slots=%d."), bLegacy);
                return Finish(&Player);
            }
            if (NextPoint == 0)
            {
                const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
                if (!Catalog || Catalog->StartingEquipment.Num() != CharacterClassCount)
                {
                    return Finish(&Player, TEXT("all eleven starting inventories must be imported"));
                }
                struct FPermission { int32 Type; uint16 Allowed; };
                const FPermission Permissions[] = {
                    {IronDagger, uint16(AllClasses & ~ClassFlag(PriestClass))},
                    {204, uint16(AllClasses & ~(ClassFlag(MageClass) | ClassFlag(PriestClass) | ClassFlag(RogueClass) | ClassFlag(MagicianClass)))},
                    {Crossbow, uint16(ClassFlag(CitizenClass) | ClassFlag(KnightClass) | ClassFlag(RogueClass) | ClassFlag(ArcherClass) | ClassFlag(SailorClass) | ClassFlag(MercenaryClass))},
                    {IronMace, uint16(AllClasses & ~(ClassFlag(MageClass) | ClassFlag(MercenaryClass) | ClassFlag(MagicianClass)))},
                    {708, uint16(ClassFlag(CitizenClass) | ClassFlag(KnightClass) | ClassFlag(PaladinClass) | ClassFlag(MercenaryClass))},
                    {704, uint16(AllClasses & ~(ClassFlag(MageClass) | ClassFlag(RogueClass) | ClassFlag(MagicianClass)))},
                    {32970, uint16(ClassFlag(MageClass) | ClassFlag(WarlockClass) | ClassFlag(PriestClass) | ClassFlag(MagicianClass))},
                };
                constexpr int32 Weapons[] = {203, 204, 200, 203, 201, 200, 28, 204, 204, 205, 200};
                for (int32 Class = 0; Class < CharacterClassCount; ++Class)
                {
                    ANativeWadPlayer* Example = Player.GetWorld()->SpawnActor<ANativeWadPlayer>();
                    bool bCorrect = Example && Example->ChooseWadClass(Class);
                    if (bCorrect)
                    {
                        bCorrect = Example->GetReadyItem() == Weapons[Class] && !Example->CanChooseWadClass()
                            && Example->GetCarriedItems().Num() == Catalog->StartingEquipment[Class].Items.Num() + 6
                            && Example->GetItemCount(StartingWaterGourd) == 1
                            && Example->GetArmorRating() == (Class == KnightClass ? 12 : 5)
                            && Example->GetAmmoReserve(0) == (Class == ArcherClass ? 80 : 0)
                            && Example->GetAmmoReserve(1) == (Class == ArcherClass ? 24 : 0)
                            && Example->GetAmmoReserve(3) == (Class == ArcherClass ? 24 : 0);
                        for (const FPermission& Permission : Permissions)
                        {
                            bCorrect &= Example->CanUseWadItem(Permission.Type) == ((Permission.Allowed & ClassFlag(Class)) != 0);
                        }
                    }
                    if (Example) { Example->Destroy(); }
                    if (!bCorrect)
                    {
                        return Finish(&Player, *FString::Printf(TEXT("starting gear or proficiency failed for %s"), CharacterClasses[Class].Name));
                    }
                }
                if (!Player.ChooseWadClass(PriestClass))
                {
                    return Finish(&Player, TEXT("could not create the controlled Priest"));
                }
                ++NextPoint;
                Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L30.L30"))));
                return true;
            }
            if (Player.GetWorld()->GetAuthGameMode<ANativeWadTownGameMode>()) { return true; }
            if (NextPoint == 1)
            {
                SendKey(Player, EKeys::I, IE_Pressed);
                ++NextPoint;
                return true;
            }
            if (NextPoint == 2)
            {
                SendKey(Player, EKeys::I, IE_Released);
                if (!Player.IsInventoryOpen()) { return Finish(&Player, TEXT("the regular inventory key did not open the equipment check")); }
                for (const int32 Type : {204, Crossbow, 708, 704})
                {
                    Player.CollectItem(Type);
                    Player.ClickInventorySlot(Player.GetCarriedItems().Find(Type));
                    Player.ClickInventorySlot(INDEX_NONE);
                }
                ++NextPoint;
                return true;
            }
            if (NextPoint == 3)
            {
                SendKey(Player, EKeys::I, IE_Pressed);
                ++NextPoint;
                return true;
            }
            SendKey(Player, EKeys::I, IE_Released);
            if (Player.GetReadyItem() != IronMace || Player.IsWornInventorySlot(Player.GetCarriedItems().Find(708))
                || !Player.IsWornInventorySlot(Player.GetCarriedItems().Find(704)) || !Player.SaveWadCheckpoint(Slot))
            {
                return Finish(&Player, TEXT("the regular equipment actions did not enforce Priest restrictions"));
            }
            auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
            if (!Save) { return Finish(&Player, TEXT("could not read the equipment checkpoint")); }
            FURL Options(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
            Options.AddOption(*FString::Printf(TEXT("WadReady=%d"), Player.GetCarriedItems().Find(204)));
            TArray<FString> Worn;
            FString(Options.GetOption(TEXT("WadWorn="), TEXT(""))).ParseIntoArray(Worn, TEXT(","), false);
            if (Worn.Num() != EquipmentSlotCount) { return Finish(&Player, TEXT("the equipment checkpoint has no worn slots")); }
            Worn[HeadSlot] = LexToString(Player.GetCarriedItems().Find(708));
            Options.AddOption(*(TEXT("WadWorn=") + FString::Join(Worn, TEXT(","))));
            Save->PlayerOptions = TEXT("?") + FString::Join(Options.Op, TEXT("?"));
            if (!UGameplayStatics::SaveGameToSlot(Save, LegacySlot, 0))
            {
                return Finish(&Player, TEXT("could not save the legacy restricted-slot fixture"));
            }
            UE_LOG(LogTemp, Display, TEXT("Verified all eleven original starting inventories, weapon and armor proficiencies, explicit useable lists, and controlled Priest equipment."));
            return Finish(&Player);
        }

        bool TickCharacter(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr int32 Mage = 2;
            constexpr const TCHAR* Slot = TEXT("NativeWadCharacterReview");
            if (bCharacterReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (Player.GetCharacterClass() != Mage || Player.GetCharacterLevel() != 2 || !Player.HasFeatherfall())
                {
                    return Finish(&Player, TEXT("the Mage, level, or active spell failed to reload"));
                }
                const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
                const FURL Options(nullptr, Save ? *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions) : TEXT(""), TRAVEL_Absolute);
                TArray<FString> Attributes;
                FString(Options.GetOption(TEXT("WadAttributes="), TEXT(""))).ParseIntoArray(Attributes, TEXT(","));
                if (Attributes.Num() != AttributeCount)
                {
                    return Finish(&Player, TEXT("the Mage checkpoint has no complete attribute list"));
                }
                for (int32 Index = 0; Index < AttributeCount; ++Index)
                {
                    if (Player.GetAttributes()[Index] != FCString::Atoi(*Attributes[Index]))
                    {
                        return Finish(&Player, TEXT("a saved Mage attribute changed during reload"));
                    }
                }
                UE_LOG(LogTemp, Display, TEXT("Verified Mage progression and Feather Fall after a fresh process reload."));
                return Finish(&Player);
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
                SendKey(Player, EKeys::LeftAlt, IE_Released);
            }
            if (FPlatformTime::Seconds() < StageTime)
            {
                return true;
            }
            StageTime = FPlatformTime::Seconds() + 0.1;
            switch (NextPoint)
            {
            case 0:
                SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                Press(Player, EKeys::N);
                break;
            case 1:
            case 2:
                Press(Player, EKeys::Left);
                break;
            case 3:
                Press(Player, EKeys::Enter);
                break;
            case 4:
                if (Player.GetCharacterClass() != Mage || Player.GetMaxHealth() != 1700 || Player.GetMaxMana() != 3500)
                {
                    return Finish(&Player, TEXT("choosing Mage did not set the authored starting stats"));
                }
                SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                Press(Player, EKeys::Q);
                break;
            case 5:
                Press(Player, EKeys::Enter);
                break;
            case 6:
                if (Current.GetAuthGameMode<ANativeWadTownGameMode>())
                {
                    if (++CaptureFrames > 20)
                    {
                        return Finish(&Player, TEXT("the selected character could not launch the first quest"));
                    }
                    Press(Player, EKeys::Enter);
                    return true;
                }
                if (!Current.GetMapName().EndsWith(TEXT("L30")))
                {
                    return Finish(&Player, TEXT("the new character's first quest went to the wrong map"));
                }
                if (Player.GetCharacterClass() != Mage)
                {
                    return Finish(&Player, TEXT("the class changed at the start of the quest"));
                }
                Press(Player, EKeys::NumPadSeven);
                break;
            case 7:
                if (Player.GetRunes() != TEXT("7") || Player.IsInventoryOpen())
                {
                    return Finish(&Player, TEXT("keypad 7 did not enter rune 7 independently of the inventory"));
                }
                Press(Player, EKeys::BackSpace);
                break;
            case 8:
                Press(Player, EKeys::NumPadFour);
                break;
            case 9:
                Press(Player, EKeys::NumPadOne);
                break;
            case 10:
                Press(Player, EKeys::RightMouseButton);
                break;
            case 11:
                if (!Player.HasFeatherfall())
                {
                    if (!Player.GetInteractionMessage().Contains(TEXT("fizzled")))
                    {
                        return Finish(&Player, TEXT("Mage Feather Fall was not recognized"));
                    }
                    NextPoint = 8;
                    return true;
                }
                Player.AwardExperience(FirstLevelThreshold);
                ++NextPoint;
                return true;
            case 12:
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { return true; }
                if (Player.GetCharacterLevel() != 2 || !Player.SaveWadCheckpoint(Slot))
                {
                    return Finish(&Player, TEXT("Mage advancement failed to save"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified initial Mage selection, quest arrival, Feather Fall, and advancement."));
                return Finish(&Player);
            }
            ++NextPoint;
            return true;
        }

        bool TickInn(UWorld& Current, ANativeWadPlayer& Player)
        {
            auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel)
            {
                return Finish(&Player, TEXT("the inn needs Town Hall's character and wallet state"));
            }
            const auto IsRested = [&]
            {
                return Player.GetHealth() == Player.GetMaxHealth() && Player.GetMana() == Player.GetMaxMana()
                    && Player.GetFood() == NativeWadRules::MaxFood && Player.GetWater() == NativeWadRules::MaxFood
                    && Player.GetPoisonLevel() == 0 && Travel->Quest.GetWalletCopper() == 0;
            };
            if (TownReview == ETownReview::InnReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (!IsRested()) { return Finish(&Player, TEXT("the paid inn stay failed to reload")); }
                UE_LOG(LogTemp, Display, TEXT("Verified the inn's restored vitals and wallet in a fresh process."));
                return Finish(&Player);
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
                SendKey(Player, EKeys::LeftAlt, IE_Released);
            }
            const auto RoomKey = [&](const FKey& Key)
            {
                SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                Press(Player, Key);
                ++NextPoint;
            };
            switch (NextPoint)
            {
            case 0:
                Travel->Quest.SetWalletCopper(3200);
                HealthBeforeTravel = Player.GetHealth();
                ManaBeforeTravel = Player.GetMana();
                RoomKey(EKeys::I);
                break;
            case 1:
                if (Player.GetInteractionMessage() != TEXT("Welcome to the inn!"))
                {
                    return Finish(&Player, TEXT("the inn could not open with Alt+I"));
                }
                RoomKey(EKeys::C);
                break;
            case 2:
                if (Travel->Quest.GetWalletCopper() != 3200 || Player.GetHealth() != HealthBeforeTravel)
                {
                    return Finish(&Player, TEXT("the commons charged money or changed health"));
                }
                RoomKey(EKeys::S);
                break;
            case 3:
                if (Travel->Quest.GetWalletCopper() != 3000 || Player.GetHealth() != HealthBeforeTravel + Player.GetMaxHealth() / 4
                    || Player.GetMana() != ManaBeforeTravel + Player.GetMaxMana() / 4 || Player.GetPoisonLevel() != 100)
                {
                    return Finish(&Player, TEXT("the small room's price or recovery differed from the room data"));
                }
                RoomKey(EKeys::L);
                break;
            case 4:
                if (Travel->Quest.GetWalletCopper() != 2000 || Player.GetPoisonLevel() != 50)
                {
                    return Finish(&Player, TEXT("the large room did not charge a platinum and halve the poison"));
                }
                RoomKey(EKeys::U);
                break;
            case 5:
                if (!IsRested()) { return Finish(&Player, TEXT("the suite did not charge two platinum and fully restore the player")); }
                RoomKey(EKeys::S);
                break;
            case 6:
                if (!IsRested() || Player.GetInteractionMessage() != TEXT("You cannot afford that room."))
                {
                    return Finish(&Player, TEXT("the inn accepted a room with insufficient funds"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified all four inn rooms, their prices and recovery, and an unaffordable rental."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickRevive(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            auto* Travel = Current.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel)
            {
                return Finish(&Player, TEXT("the revive review requires saved player finances"));
            }
            const bool bHasDrops = [&]
            {
                bool bDagger = false;
                bool bQuiver = false;
                int32 CoinPiles = 0;
                for (TActorIterator<ANativeWadPickup> Item(&Current); Item; ++Item)
                {
                    bDagger |= Item->IsDropped() && Item->GetItemType() == IronDagger;
                    bQuiver |= Item->IsDropped() && Item->GetItemType() == CrossbowAmmo[1].Quiver && Item->GetAmmoQuantity() == 8;
                    CoinPiles += Item->IsDropped() && CoinValueCopper(Item->GetItemType()) > 0;
                }
                return bDagger && bQuiver && CoinPiles == 4 && Travel->Quest.GetWalletCopper() == 0 && Player.GetItemCount(300) == 1;
            }();
            if (bReviveReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (!bHasDrops || Player.GetHealth() != Player.GetMaxHealth() || Player.GetReadyItem() != 0)
                {
                    return Finish(&Player, TEXT("revival or dropped equipment was missing from the checkpoint"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the revived player, retained rune, and dropped gear and coins across a process restart."));
                return Finish(&Player);
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            switch (NextPoint)
            {
            case 0:
            {
                const TActorIterator<ANativeWadLevel> Level(&Current);
                const TActorIterator<ANativeWadGuardian> Enemy(&Current);
                if (!Level || !Enemy || !Player.CollectItem(IronDagger) || !Player.CollectAmmo(1, 8) || !Player.CollectItem(300))
                {
                    return Finish(&Player, TEXT("could not set up the revive fixture"));
                }
                Travel->Quest.SetWalletCopper(6789);
                ReviveEntrance = Player.GetActorLocation();
                if (!PlaceReviewPlayer(**Level, Player, FVector2D(Enemy->GetActorLocation()) + FVector2D(-150, 0)))
                {
                    return Finish(&Player, TEXT("could not place the revive fixture outside the entrance"));
                }
                Player.ReceiveTrapPoison(Player.GetHealth());
                ++NextPoint;
                break;
            }
            case 1:
                if (!bHasDrops || Player.GetHealth() != 0 || Player.GetAmmoReserve(1) != 0)
                {
                    return Finish(&Player, TEXT("defeat did not leave the dagger and ammo in the level"));
                }
                Press(Player, EKeys::SpaceBar);
                ++NextPoint;
                break;
            case 2:
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                if (!bHasDrops || !FVector2D(Player.GetActorLocation()).Equals(FVector2D(ReviveEntrance), 3)
                    || Player.GetHealth() != Player.GetMaxHealth() || Player.GetMana() != Player.GetMaxMana() / 2
                    || Player.GetPoisonLevel() != 0)
                {
                    return Finish(&Player, TEXT("Space failed to restore the original revive state"));
                }
                if (!Player.SaveWadCheckpoint(TEXT("NativeWadReviveReview"))) { break; }
                UE_LOG(LogTemp, Display, TEXT("Verified defeat, equipment drops, Space revival, and a new checkpoint."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickAmmoBanner(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr int32 Index = 2;
            constexpr int32 Quantity = 36;
            const TActorIterator<ANativeWadLevel> Level(&Current);
            APlayerController* Controller = Player.GetController<APlayerController>();
            ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
            if (!Level || !Hud || !Hud->HasClassicHud() || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the authored quiver review needs L20 and its classic HUD"));
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const auto Click = [&](FVector2D Point)
            {
                int32 Width = 0, Height = 0;
                Controller->GetViewportSize(Width, Height);
                const double Scale = FMath::Min(Width / 320.0, Height / 200.0);
                Hud->ClickHUD((FVector2D(Width, Height) - FVector2D(320, 200) * Scale) * 0.5 + Scale * Point);
            };
            switch (NextPoint)
            {
            case 0:
                for (TActorIterator<ANativeWadPickup> Item(&Current); Item; ++Item)
                {
                    if (!Item->ActorHasTag(TEXT("WAD Item Thing 163")))
                    {
                        continue;
                    }
                    const FVector Position = Item->GetActorLocation();
                    if (Item->GetItemType() != CrossbowAmmo[Index].Quiver || Item->GetAmmoQuantity() != Quantity
                        || !PlaceReviewPlayer(**Level, Player, FVector2D(Position) + FVector2D(35, 0)))
                    {
                        return Finish(&Player, TEXT("L20's authored piercing quiver and quantity are missing"));
                    }
                    Aim(Player, Position);
                    Press(Player, EKeys::E);
                    ++NextPoint;
                    return true;
                }
                return Finish(&Player, TEXT("could not find the original L20 quiver"));
            case 1:
                if (Player.GetAmmoReserve(Index) != Quantity)
                {
                    return Finish(&Player, TEXT("E did not collect the authored quantity of piercing bolts"));
                }
                Click({13, 190});
                Click({305, 59});
                Click({230, 68});
                if (!Player.IsInventoryOpen() || Player.GetSelectedAmmo() != Index
                    || Player.GetAmmoReserve(Index) != Quantity - NormalQuiverBolts
                    || Player.GetItemCount(CrossbowAmmo[Index].Quiver) != 1)
                {
                    return Finish(&Player, TEXT("the classic banner did not select and retrieve piercing ammo"));
                }
                ++NextPoint;
                break;
            case 2:
            {
                bool bSaved = false;
                if (!CaptureTownFrame(TEXT("ammo"), bSaved)) { break; }
                if (!bSaved) { return Finish(&Player, TEXT("could not save the rendered ammo banner")); }
                Click({309, 9});
                if (Player.IsInventoryOpen() || !Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                Press(Player, EKeys::F5);
                ++NextPoint;
                break;
            }
            case 3:
                if (Player.GetInteractionMessage() != TEXT("Checkpoint saved")) { break; }
                Press(Player, EKeys::F9);
                ++NextPoint;
                break;
            case 4:
                if (!Player.HasRestoredWadCheckpoint()) { break; }
                if (Player.GetSelectedAmmo() != Index || Player.GetAmmoReserve(Index) != Quantity - NormalQuiverBolts
                    || Player.GetItemCount(CrossbowAmmo[Index].Quiver) != 1)
                {
                    return Finish(&Player, TEXT("normal save and load did not restore the quiver and ammo selection"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified L20's 36-bolt quiver, classic ammo selection and retrieval, and F5/F9 reload."));
                return Finish(&Player);
            }
            return true;
        }

        bool LaunchIncomingReviewMissile(ANativeWadPlayer& Player, uint8 DamageMask)
        {
            UWorld& Current = *Player.GetWorld();
            const FVector End = Player.GetActorLocation();
            for (int32 Angle = 0; Angle < 8; ++Angle)
            {
                const FVector Offset = FRotator(0, Angle * 45, 0).Vector() * 40.0;
                const FVector Start = End + Offset;
                FHitResult Hit;
                if (!Current.SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
                    FCollisionShape::MakeSphere(5.0f), FCollisionQueryParams(SCENE_QUERY_STAT(ControlWandIncoming), true))
                    || Hit.GetActor() != &Player) { continue; }
                ANativeWadBolt* Bolt = Current.SpawnActor<ANativeWadBolt>(Start, FRotator::ZeroRotator);
                if (!Bolt) { return false; }
                Bolt->Launch(nullptr, nullptr, -Offset.GetSafeNormal() * 2000, 100);
                Bolt->SetDamageMask(DamageMask);
                return true;
            }
            return false;
        }

        bool TickDispelEffects(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            UWorld& Current = *Player.GetWorld();
            if (HeldKey) { SendKey(Player, HeldKey.GetValue(), IE_Released); HeldKey.Reset(); }
            const auto Count = [&]()
            {
                return int32(Player.GetStatusPower(InvisibleStatus) != 0) + int32(Player.GetStatusPower(StrengthStatus) != 0)
                    + int32(Player.GetStatusPower(AcidResistStatus) != 0);
            };
            const auto Protected = [&]()
            {
                return Player.GetReadyItem() == 606 && (Player.GetResistanceMask() & DamageFlag(EProjectileDamage::Poison)) != 0;
            };
            if (bDispelReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (Count() != 0 || !Protected() || Player.GetItemCount(606) != 1 || Player.GetHealth() != 30000 || Player.GetMana() != 10000)
                { return Finish(&Player, TEXT("dispelled effects or the protected equipped effect failed to reload")); }
                UE_LOG(LogTemp, Display, TEXT("Verified removed temporary magic stays removed and equipped protection survives Dispel across a process restart."));
                return Finish(&Player);
            }
            switch (NextPoint)
            {
            case 0:
                for (TActorIterator<ANativeWadGuardian> Enemy(&Current); Enemy; ++Enemy)
                { Enemy->SetActorTickEnabled(false); Enemy->GetCharacterMovement()->DisableMovement(); }
                if (Count() != 3) { return Finish(&Player, TEXT("the Dispel fixture requires three supplied temporary effects")); }
                HealthBeforeCast = Player.GetHealth();
                ExperienceBeforeTravel = Player.GetExperience();
                ++NextPoint;
                break;
            case 1:
                if (Player.GetReadyItem() != 606 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 606); break; }
                DispelEffectsBefore = Count();
                if (!Protected() || !LaunchIncomingReviewMissile(Player, SpecialDamageFlag(ESpecialDamage::Dispel)))
                { return Finish(&Player, TEXT("could not launch Dispel at the protected player")); }
                StageTime = Current.GetTimeSeconds();
                ++NextPoint;
                break;
            case 2:
                if (Current.GetTimeSeconds() - StageTime < 0.15) { break; }
                if (Count() != FMath::Max(0, DispelEffectsBefore - 1) || !Protected() || Player.GetHealth() != HealthBeforeCast
                    || Player.GetMana() != 10000 || Player.GetExperience() != ExperienceBeforeTravel || Player.GetItemCount(606) != 1)
                { return Finish(&Player, TEXT("Dispel did not remove exactly one temporary effect while preserving equipped protection, vitals, and XP")); }
                UE_LOG(LogTemp, Display, TEXT("Verified incoming Dispel reduces temporary effect count %d to %d while preserving equipped poison protection."), DispelEffectsBefore, Count());
                if (DispelEffectsBefore > 0) { NextPoint = 1; break; }
                ++NextPoint;
                break;
            case 3:
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                if (!Player.SaveWadCheckpoint(TEXT("NativeWadDispelEffectsReview")))
                { return Finish(&Player, TEXT("could not save the Dispel result")); }
                return Finish(&Player);
            }
            return true;
        }

        bool TickControlWands(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            UWorld& Current = *Player.GetWorld();
            if (HeldKey) { SendKey(Player, HeldKey.GetValue(), IE_Released); HeldKey.Reset(); }
            ANativeWadGuardian* Target = ElementalTarget.Get();
            const auto Incoming = [&](ESpecialDamage Effect) { return LaunchIncomingReviewMissile(Player, SpecialDamageFlag(Effect)); };
            if (NextPoint == 0)
            {
                for (TActorIterator<ANativeWadGuardian> Enemy(&Current); Enemy; ++Enemy)
                {
                    if (!Target && Enemy->GetSourceType() == 1023 && !Enemy->IsHidden()) { Target = *Enemy; ElementalTarget = Target; }
                    if (*Enemy != Target) { Enemy->SetActorTickEnabled(false); Enemy->GetCharacterMovement()->DisableMovement(); }
                }
                if (!Target) { return Finish(&Player, TEXT("the control-wand fixture requires an original L61 Griffon")); }
                if (bControlWandReload)
                {
                    if (!Player.HasRestoredWadCheckpoint()) { return true; }
                    if (Target->GetSlowSeconds() <= 25.0 || Target->GetEarthboundSeconds() <= 25.0
                        || Target->GetCharacterMovement()->IsFlying() || Player.CanFly() || Player.GetReadyItem() != FlyingAmulet
                        || Player.GetStatusPower(SpeedStatus) != -20)
                    { return Finish(&Player, TEXT("creature controls, player Slow, or suppressed flight failed to restore")); }
                    UE_LOG(LogTemp, Display, TEXT("Verified saved creature Slow/Earthbind and the player's Slow and suppressed readied flight across a process restart."));
                    // Controlled expiry check after preserving and verifying the actual source durations.
                    Target->RestoreControlEffects(0.25, 0.25);
                    StageTime = Current.GetTimeSeconds();
                    NextPoint = 20;
                    return true;
                }
                const TActorIterator<ANativeWadLevel> Level(&Current);
                const FVector Position = Target->GetActorLocation();
                if (!Level || !PlaceReviewPlayer(**Level, Player, FVector2D(Position) + FVector2D(-180, 0)))
                { return Finish(&Player, TEXT("could not use the source Griffon room")); }
                FWadEncounterProfile Profile;
                Profile.Name = TEXT("Controlled source-flying Griffon");
                Profile.SourceType = 1023;
                Profile.MaxHealth = 5000;
                Profile.WalkSpeed = 0;
                Profile.MeleeDamage = Profile.BoltDamage = 0;
                Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
                Target->RestoreEncounterHealth(5000);
                Target->SetActorLocation(Position + FVector(0, 0, 100), false, nullptr, ETeleportType::TeleportPhysics);
                DriftStart = Target->GetActorLocation();
                ExperienceBeforeTravel = Player.GetExperience();
                HealthBeforeCast = Player.GetHealth();
                ++NextPoint;
            }
            switch (NextPoint)
            {
            case 1:
                if (Player.GetReadyItem() != 911 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 911); break; }
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Current.GetTimeSeconds();
                ++NextPoint;
                break;
            case 2:
                if (Current.GetTimeSeconds() - StageTime < 1.0) { break; }
                if (Target->GetEarthboundSeconds() <= 25.0 || Target->GetCharacterMovement()->IsFlying()
                    || Target->GetActorLocation().Z >= DriftStart.Z - 40.0 || Target->GetEncounterHealth() != 5000)
                { return Finish(&Player, TEXT("Earthbind did not bring down the flying target without health damage")); }
                UE_LOG(LogTemp, Display, TEXT("Verified Earthbind wand 911 brings down the flying Griffon without health damage."));
                ++NextPoint;
                break;
            case 3:
                if (Player.GetReadyItem() != 917 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 917); break; }
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Current.GetTimeSeconds();
                ++NextPoint;
                break;
            case 4:
                if (Current.GetTimeSeconds() - StageTime < 0.3) { break; }
                if (Target->GetSlowSeconds() <= 25.0 || !FMath::IsNearlyEqual(Target->GetCharacterMovement()->MaxWalkSpeed, 17.5f)
                    || Player.GetExperience() != ExperienceBeforeTravel || Target->GetEncounterHealth() != 5000
                    || Player.GetMana() != 9200 || Player.GetItemCount(911) != 1 || Player.GetItemCount(917) != 1)
                { return Finish(&Player, TEXT("Slow/Earthbind had the wrong speed, damage, XP, mana, or retained items")); }
                if (!Incoming(ESpecialDamage::Slow)) { return Finish(&Player, TEXT("could not launch player Slow check")); }
                StageTime = Current.GetTimeSeconds();
                ++NextPoint;
                break;
            case 5:
                if (Current.GetTimeSeconds() - StageTime < 0.15) { break; }
                if (Player.GetStatusPower(SpeedStatus) != -20 || Player.GetHealth() != HealthBeforeCast
                    || !FMath::IsNearlyEqual(Player.GetCharacterMovement()->MaxWalkSpeed, 190.0f * 10.0f / 14.0f, 0.1f))
                { return Finish(&Player, *FString::Printf(TEXT("incoming Slow has power %d, health %d/%d, movement %.3f, base speed %d"), Player.GetStatusPower(SpeedStatus), Player.GetHealth(), HealthBeforeCast, Player.GetCharacterMovement()->MaxWalkSpeed, Player.GetAttributes()[SpeedAttribute])); }
                ++NextPoint;
                break;
            case 6:
                if (Player.GetReadyItem() != FlyingAmulet || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, FlyingAmulet); break; }
                if (!Player.CanFly()) { return Finish(&Player, TEXT("the readied source amulet did not grant flight")); }
                if (!Incoming(ESpecialDamage::Earthbind)) { return Finish(&Player, TEXT("could not launch player Earthbind check")); }
                StageTime = Current.GetTimeSeconds();
                ++NextPoint;
                break;
            case 7:
                if (Current.GetTimeSeconds() - StageTime < 0.3 || !Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                if (Player.CanFly() || Player.GetReadyItem() != FlyingAmulet || Player.GetHealth() != HealthBeforeCast)
                { return Finish(&Player, TEXT("Earthbind failed to suppress the readied amulet without consuming or damaging the player")); }
                if (!Player.SaveWadCheckpoint(TEXT("NativeWadControlWandsReview"))) { return Finish(&Player, TEXT("could not save the source control effects")); }
                UE_LOG(LogTemp, Display, TEXT("Verified Slow/Earthbind wands, source duration bounds, retained items, player modifiers, and suppressed readied flight; saved effects."));
                return Finish(&Player);
            case 20:
                if (Current.GetTimeSeconds() - StageTime < 0.4) { break; }
                if (Target->GetSlowSeconds() != 0.0 || Target->GetEarthboundSeconds() != 0.0
                    || !Target->GetCharacterMovement()->IsFlying() || Target->GetCharacterMovement()->MaxFlySpeed != 105.0f)
                { return Finish(&Player, TEXT("creature control expiry did not restore source flight and speed")); }
                Target->SetActorTickEnabled(false);
                Target->GetCharacterMovement()->DisableMovement();
                ++NextPoint;
                break;
            case 21:
                if (Player.GetStatusPower(SpeedStatus) != 0) { break; }
                if (Player.GetCharacterMovement()->MaxWalkSpeed != 190.0f || Player.CanFly())
                { return Finish(&Player, TEXT("player Slow expiry changed the baseline or re-enabled suppressed flight")); }
                if (Player.GetReadyItem() != 911 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 911); break; }
                ++NextPoint;
                break;
            case 22:
                if (Player.GetReadyItem() != FlyingAmulet || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, FlyingAmulet); break; }
                if (!Player.CanFly()) { return Finish(&Player, TEXT("unreadying and readying the amulet did not restore its source effect")); }
                Target->RestoreEncounterHealth(0);
                Target->ResetForGenerator();
                if (Target->GetSlowSeconds() != 0.0 || Target->GetEarthboundSeconds() != 0.0 || !Target->GetCharacterMovement()->IsFlying())
                { return Finish(&Player, TEXT("a reused generator retained a control effect")); }
                UE_LOG(LogTemp, Display, TEXT("Verified controlled creature expiry, original player Slow expiry, amulet reactivation, and cleared effects on generator reuse."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickRuneProjectiles(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            TArray<const FWadSpell*> Spells;
            for (const FWadSpell& Spell : SupportedSpells)
            {
                if (Spell.Effect == ESpellEffect::Projectile && Spell.School == SchoolForClass(Player.GetCharacterClass())) { Spells.Add(&Spell); }
            }
            if (Spells.IsEmpty()) { return Finish(&Player, TEXT("the rune projectile review has no spells for this class")); }
            if (bRuneProjectileReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                for (int32 Rune = 1; Rune <= 9; ++Rune)
                {
                    if (!Player.HasRune(Rune)) { return Finish(&Player, TEXT("a source rune failed to reload after projectile casting")); }
                }
                if (Player.GetSuccessfulCasts() != Spells.Num() || Player.GetMana() != Spells.Last()->Mana)
                { return Finish(&Player, TEXT("projectile-spell cast count or refusal mana failed to reload")); }
                UE_LOG(LogTemp, Display, TEXT("Verified %d projectile spells, owned runes, and exact-cost refusal mana after restart for class %d."), Spells.Num(), Player.GetCharacterClass());
                return Finish(&Player);
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const FWadSpell& Spell = *Spells[RuneProjectileIndex];
            const FWadWandProjectile* Rules = FindWandProjectile(Spell.ProjectileIndex);
            if (!Rules) { return Finish(&Player, TEXT("a supported spell has no source missile")); }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            ANativeWadGuardian* Target = ElementalTarget.Get();
            const int32 Expected = (Rules->DamageMask & SpecialDamage) ? 0
                : Rules->Damage + ((Rules->DamageMask & DamageFlag(EProjectileDamage::Fire)) ? Rules->Damage / 4 : 0);
            const auto TypeRunes = [&]()
            {
                const FString Code(Spell.Runes);
                const FString Entered = Player.GetRunes();
                if (!Entered.IsEmpty() && !Code.StartsWith(Entered)) { Press(Player, EKeys::BackSpace); return false; }
                if (Entered != Code)
                {
                    Press(Player, ReviewRuneKeys[Code[Entered.Len()] - TEXT('1')]);
                    return false;
                }
                return true;
            };
            switch (NextPoint)
            {
            case 0:
            {
                const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
                const TActorIterator<ANativeWadGuardian> Original(Player.GetWorld());
                if (!Level || !Original) { return Finish(&Player, TEXT("the rune projectile fixture needs the original L30 room")); }
                const FVector Position = Original->GetActorLocation();
                for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                {
                    Enemy->SetActorTickEnabled(false);
                    Enemy->SetActorEnableCollision(false);
                    Enemy->SetActorHiddenInGame(true);
                }
                if (!PlaceReviewPlayer(**Level, Player, FVector2D(Position) + FVector2D(-150, 0)))
                { return Finish(&Player, TEXT("could not use the original room for rune casting")); }
                const FTransform Transform(FRotator::ZeroRotator, Position);
                Target = Player.GetWorld()->SpawnActorDeferred<ANativeWadGuardian>(ANativeWadGuardian::StaticClass(), Transform,
                    nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
                if (!Target) { return Finish(&Player, TEXT("could not create the controlled spell target")); }
                FWadEncounterProfile Profile;
                Profile.Name = TEXT("Controlled rune-projectile target");
                Profile.MaxHealth = 10000;
                Profile.RegenPerUpdate = 0;
                Profile.WalkSpeed = 0;
                Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
                UGameplayStatics::FinishSpawningActor(Target, Transform);
                Target->SetActorTickEnabled(false);
                ElementalTarget = Target;
                DriftStart = Position;
                ++NextPoint;
                break;
            }
            case 1:
                if (Now < StageTime) { break; }
                Target->SetActorLocation(DriftStart, false, nullptr, ETeleportType::TeleportPhysics);
                Target->GetCharacterMovement()->StopMovementImmediately();
                Target->RestoreEncounterHealth(10000);
                Target->RestorePoison(0, 0);
                Target->RestoreControlEffects(0, 0);
                Player.RestoreWadVital(9, Player.GetMaxHealth());
                Player.RestoreWadVital(10, 2 * Spell.Mana - Player.GetMana());
                ExperienceBeforeTravel = Player.GetExperience();
                RuneProjectileCastsBefore = Player.GetSuccessfulCasts();
                ++NextPoint;
                break;
            case 2:
                if (!TypeRunes()) { break; }
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::RightMouseButton);
                StageTime = Now + 0.5;
                ++NextPoint;
                break;
            case 3:
                if (Now < StageTime) { break; }
                if (Target->GetEncounterHealth() != 10000 - Expected || Player.GetExperience() != ExperienceBeforeTravel + Expected
                    || Player.GetMana() != Spell.Mana || Player.GetSuccessfulCasts() != RuneProjectileCastsBefore + 1)
                { return Finish(&Player, *FString::Printf(TEXT("rune spell %s has target %d, mana %d, casts %d instead of %d"), Spell.Name,
                    Target->GetEncounterHealth(), Player.GetMana(), Player.GetSuccessfulCasts(), RuneProjectileCastsBefore + 1)); }
                ++NextPoint;
                break;
            case 4:
                if (!TypeRunes()) { break; }
                Press(Player, EKeys::RightMouseButton);
                ++NextPoint;
                break;
            case 5:
                if (Target->GetEncounterHealth() != 10000 - Expected || Player.GetMana() != Spell.Mana
                    || Player.GetSuccessfulCasts() != RuneProjectileCastsBefore + 1)
                { return Finish(&Player, TEXT("a rune projectile spell spent mana or fired at exactly its cost")); }
                for (TActorIterator<ANativeWadBolt> Bolt(Player.GetWorld()); Bolt; ++Bolt)
                {
                    if (Bolt->GetOwner() == &Player && !Bolt->IsActorBeingDestroyed())
                    { return Finish(&Player, TEXT("an exact-cost spell refusal spawned a missile")); }
                }
                UE_LOG(LogTemp, Display, TEXT("Verified class %d spell %s (%s), source missile %d, damage or utility, XP, mana, and exact-cost refusal."),
                    Player.GetCharacterClass(), Spell.Name, Spell.Runes, Rules->Type);
                if (++RuneProjectileIndex < Spells.Num()) { StageTime = Now + 0.25; NextPoint = 1; break; }
                --RuneProjectileIndex;
                ++NextPoint;
                break;
            case 6:
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                if (!Player.SaveWadCheckpoint(TEXT("NativeWadRuneProjectilesReview")))
                { return Finish(&Player, TEXT("could not save the owned runes and projectile spell result")); }
                return Finish(&Player);
            }
            return true;
        }

        bool TickConfusionWand(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            if (bConfusionWandReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (Player.GetReadyItem() != 913 || Player.GetItemCount(913) != 1 || Player.GetMana() != 100)
                { return Finish(&Player, TEXT("the confusion wand, ready hand, or refusal mana failed to reload")); }
                UE_LOG(LogTemp, Display, TEXT("Verified the confusion wand and remaining mana across a process restart."));
                return Finish(&Player);
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            ANativeWadGuardian* Target = ElementalTarget.Get();
            switch (NextPoint)
            {
            case 0:
            {
                const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
                const TActorIterator<ANativeWadGuardian> Original(Player.GetWorld());
                if (!Level || !Original || !PlaceReviewPlayer(**Level, Player, FVector2D(Original->GetActorLocation()) + FVector2D(-150, 0)))
                { return Finish(&Player, TEXT("the confusion fixture needs the original L30 encounter room")); }
                Target = *Original;
                ElementalTarget = Target;
                Target->SetActorRotation((Player.GetActorLocation() - Target->GetActorLocation()).Rotation());
                StageTime = Player.GetWorld()->GetTimeSeconds();
                ++NextPoint;
                break;
            }
            case 1:
                if (!Target->HasAcquiredTarget())
                {
                    if (Player.GetWorld()->GetTimeSeconds() - StageTime > 1.0)
                    { return Finish(&Player, TEXT("the confusion target did not acquire the controlled player approach")); }
                    break;
                }
                Target->SetActorTickEnabled(false);
                Target->GetCharacterMovement()->DisableMovement();
                ConfusionHeading = Target->GetActorRotation();
                HealthBeforeCast = Target->GetEncounterHealth();
                ExperienceBeforeTravel = Player.GetExperience();
                ++NextPoint;
                break;
            case 2:
                if (Player.GetReadyItem() != 913 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 913); break; }
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Player.GetWorld()->GetTimeSeconds();
                ++NextPoint;
                break;
            case 3:
                if (Player.GetWorld()->GetTimeSeconds() - StageTime < 0.3) { break; }
                if (Target->HasAcquiredTarget() || Target->GetActorRotation().Equals(ConfusionHeading, 0.001)
                    || Target->GetEncounterHealth() != HealthBeforeCast || Target->GetPoisonLevel() != 0
                    || Player.GetExperience() != ExperienceBeforeTravel || Player.GetMana() != 200 || Player.GetItemCount(913) != 1)
                { return Finish(&Player, TEXT("confusion failed to clear and turn the acquired target without health damage or XP")); }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 4:
                if (Player.GetMana() != 100 || Player.GetItemCount(913) != 1)
                { return Finish(&Player, TEXT("the confusion wand failed its exact-cost refusal")); }
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                if (!Player.SaveWadCheckpoint(TEXT("NativeWadConfusionWandReview")))
                { return Finish(&Player, TEXT("could not save the confusion wand result")); }
                UE_LOG(LogTemp, Display, TEXT("Verified original wand 913 missile 4010: acquired-target loss, random heading, no damage or XP, mana, refusal, and retained item."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickResistanceItems(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            UWorld& Current = *Player.GetWorld();
            constexpr const TCHAR* Slot = TEXT("NativeWadResistanceItemsReview");
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            if (bResistanceReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (NextPoint == 0)
                {
                    const uint8 Expected = DamageFlag(EProjectileDamage::Fire) | DamageFlag(EProjectileDamage::Poison)
                        | DamageFlag(EProjectileDamage::ManaDrain);
                    if (Player.GetResistanceMask() != Expected || Player.GetReadyItem() != 618)
                    { return Finish(&Player, TEXT("the ready amulet and active resistance scrolls did not restore")); }
                    ++NextPoint;
                }
                else if (NextPoint == 1)
                {
                    if (Player.GetReadyItem() != IronMace || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, IronMace); return true; }
                    StageTime = Current.GetTimeSeconds();
                    ++NextPoint;
                }
                else
                {
                    if (Player.GetResistanceMask() & DamageFlag(EProjectileDamage::ManaDrain))
                    { return Finish(&Player, TEXT("the amulet remained active after unreadying it")); }
                    const double Elapsed = Current.GetTimeSeconds() - StageTime;
                    if (Player.GetResistanceMask() == 0)
                    {
                        if (Elapsed < 15000.0 / TicksPerSecond - 3.0)
                        { return Finish(&Player, TEXT("the resistance scrolls expired before their source duration")); }
                        UE_LOG(LogTemp, Display, TEXT("Verified resistance scroll and readied-amulet restart, unreadying, and original expiration."));
                        return Finish(&Player);
                    }
                    if (Elapsed > 15000.0 / TicksPerSecond + 1.0)
                    { return Finish(&Player, TEXT("the resistance scrolls outlasted their source duration")); }
                }
                return true;
            }
            struct FResistanceItem { int32 Type; EProjectileDamage Element; };
            constexpr FResistanceItem Amulets[] = {
                {606, EProjectileDamage::Poison}, {607, EProjectileDamage::Fire}, {616, EProjectileDamage::Acid},
                {617, EProjectileDamage::Electricity}, {618, EProjectileDamage::ManaDrain},
            };
            if (NextPoint == 0)
            {
                for (TActorIterator<ANativeWadGuardian> Enemy(&Current); Enemy; ++Enemy)
                {
                    Enemy->SetActorTickEnabled(false);
                    Enemy->GetCharacterMovement()->DisableMovement();
                }
                if (Player.GetArmorRating() != 0) { return Finish(&Player, TEXT("the controlled resistance fixture requires no armor")); }
                ++NextPoint;
            }
            const int32 Index = (NextPoint - 1) / 4;
            if (Index < UE_ARRAY_COUNT(Amulets))
            {
                const auto& Item = Amulets[Index];
                const int32 Phase = (NextPoint - 1) % 4;
                if (Phase == 0 || Phase == 2)
                {
                    const int32 Ready = Phase == 0 ? Item.Type : IronMace;
                    if (Player.GetReadyItem() != Ready || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, Ready); return true; }
                    const uint8 ExpectedMask = Phase == 0 ? DamageFlag(Item.Element) : 0;
                    if (Player.GetResistanceMask() != ExpectedMask) { return Finish(&Player, *FString::Printf(TEXT("amulet %d has resistance %d instead of %d"), Ready, Player.GetResistanceMask(), ExpectedMask)); }
                    HealthBeforeCast = Player.GetHealth();
                    ResistanceManaBefore = Player.GetMana();
                    ResistancePoisonBefore = Player.GetPoisonLevel();
                    ANativeWadBolt* Bolt = Current.SpawnActor<ANativeWadBolt>(Player.GetActorLocation() + FVector(-100, 0, 0), FRotator::ZeroRotator);
                    if (!Bolt) { return Finish(&Player, TEXT("could not launch the controlled incoming missile")); }
                    Bolt->Launch(nullptr, nullptr, FVector(2000, 0, 0), 400);
                    Bolt->SetDamageMask(DamageFlag(Item.Element));
                    StageTime = Current.GetTimeSeconds();
                    ++NextPoint;
                }
                else if (Current.GetTimeSeconds() - StageTime >= 0.1)
                {
                    const bool Protected = Phase == 1;
                    const int32 ExpectedDamage = Protected ? 80 : Item.Element == EProjectileDamage::Fire ? 500 : 400;
                    const int32 ExpectedPoison = Item.Element == EProjectileDamage::Poison ? Protected ? 10 : 40 : 0;
                    const int32 ExpectedDrain = Item.Element == EProjectileDamage::ManaDrain ? Protected ? 100 : 400 : 0;
                    if (Player.GetHealth() != HealthBeforeCast - ExpectedDamage
                        || Player.GetPoisonLevel() != ResistancePoisonBefore + ExpectedPoison
                        || Player.GetMana() != ResistanceManaBefore - ExpectedDrain)
                    { return Finish(&Player, *FString::Printf(TEXT("incoming element %d had the wrong health, poison, or mana result (protected %d, damage %d, poison %d, mana %d)"),
                        int32(Item.Element), Protected, HealthBeforeCast - Player.GetHealth(), Player.GetPoisonLevel() - ResistancePoisonBefore, ResistanceManaBefore - Player.GetMana())); }
                    if (!Protected) { UE_LOG(LogTemp, Display, TEXT("Verified amulet %d against actual incoming missiles, including unreadying."), Item.Type); }
                    ++NextPoint;
                }
                return true;
            }
            const int32 ScrollStep = NextPoint - 1 - UE_ARRAY_COUNT(Amulets) * 4;
            if (ScrollStep < 4)
            {
                const int32 Type = ScrollStep < 2 ? 353 : 352;
                if (ScrollStep % 2 == 0)
                {
                    if (Player.GetReadyItem() != Type || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, Type); return true; }
                    Press(Player, EKeys::LeftControl);
                }
                else if (Player.GetItemCount(Type) != 0 || Player.GetStatusPower(Type == 353 ? FireResistStatus : PoisonResistStatus) != 1)
                { return Finish(&Player, *FString::Printf(TEXT("resistance scroll %d has count %d and status %d"), Type, Player.GetItemCount(Type), Player.GetStatusPower(Type == 353 ? FireResistStatus : PoisonResistStatus))); }
                ++NextPoint;
                return true;
            }
            if (Player.GetReadyItem() != 618 || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, 618); return true; }
            if (!Player.GetCharacterMovement()->IsMovingOnGround()) { return true; }
            if (!Player.SaveWadCheckpoint(Slot)) { return Finish(&Player, TEXT("could not save the resistance item effects")); }
            UE_LOG(LogTemp, Display, TEXT("Verified five original resistance amulets with live missiles and both consumed resistance scrolls; saved active effects."));
            return Finish(&Player);
        }

        bool TickUtilityItems(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr const TCHAR* Slot = TEXT("NativeWadUtilityItemsReview");
            struct FTestItem { int32 Type; EItemStatus Effect; int32 Power; };
            constexpr FTestItem Items[] = {
                {813, LavaWalkStatus, 1}, {815, InvisibleStatus, 1},
                {816, TranslucentStatus, 1}, {820, StrengthStatus, 20},
            };
            if (bUtilityReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                bool bActive = true;
                bool bExpired = true;
                for (const FTestItem& Item : Items)
                {
                    const int32 Power = Player.GetStatusPower(Item.Effect);
                    bActive &= Power == Item.Power;
                    bExpired &= Power == 0;
                }
                if (NextPoint == 0)
                {
                    if (!bActive) { return Finish(&Player, TEXT("the original potion effects failed to reload")); }
                    StageTime = Current.GetTimeSeconds();
                    ++NextPoint;
                }
                else if (bExpired)
                {
                    const double Elapsed = Current.GetTimeSeconds() - StageTime;
                    if (Elapsed < 5000.0 / TicksPerSecond - 2.0)
                    {
                        return Finish(&Player, TEXT("the potion effects expired too soon"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified all four utility potion effects reload and expire."));
                    return Finish(&Player);
                }
                else if (Current.GetTimeSeconds() - StageTime > 5000.0 / TicksPerSecond + 1.0)
                {
                    return Finish(&Player, TEXT("the potion effects lasted past their authored duration"));
                }
                return true;
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const int32 Index = NextPoint / 2;
            if (Index == UE_ARRAY_COUNT(Items))
            {
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { return true; }
                if (!Player.SaveWadCheckpoint(Slot)) { return Finish(&Player, TEXT("could not save the active potion effects")); }
                UE_LOG(LogTemp, Display, TEXT("Verified use and consumption of Lava Walking, Invisibility, Translucency, and Demon Strength potions."));
                return Finish(&Player);
            }
            const FTestItem& Item = Items[Index];
            if (NextPoint % 2 == 0)
            {
                if (!Player.GetItemCount(Item.Type)) { return Finish(&Player, TEXT("the potion review needs its authored items")); }
                if (Player.GetReadyItem() != Item.Type || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, Item.Type);
                    return true;
                }
                Press(Player, EKeys::LeftControl);
            }
            else if (Player.GetItemCount(Item.Type) || Player.GetStatusPower(Item.Effect) != Item.Power)
            {
                return Finish(&Player, *FString::Printf(TEXT("potion %d failed to apply its authored effect and be consumed"), Item.Type));
            }
            ++NextPoint;
            return true;
        }

        bool TickSpecialAmmo(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr const TCHAR* Slot = TEXT("NativeWadSpecialAmmoReview");
            constexpr int32 Last = UE_ARRAY_COUNT(CrossbowAmmo) - 1;
            if (bAmmoReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                if (Player.GetSelectedAmmo() != Last || Player.GetAmmoReserve(Last) != 8 || Player.GetReadyItem() != Crossbow)
                {
                    return Finish(&Player, TEXT("the ammo selection, reserve, or ready crossbow failed to reload"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified ammo selection and reserve across a process restart."));
                return Finish(&Player);
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            if (NextPoint == 0)
            {
                const TActorIterator<ANativeWadLevel> Level(&Current);
                const TActorIterator<ANativeWadGuardian> Existing(&Current);
                if (!Level || !Existing || !Player.CollectItem(Crossbow))
                {
                    return Finish(&Player, TEXT("the special-ammo review needs its level, encounter, and crossbow"));
                }
                const FTransform Transform = Existing->GetActorTransform();
                Existing->Destroy();
                ANativeWadGuardian* Target = Current.SpawnActorDeferred<ANativeWadGuardian>(ANativeWadGuardian::StaticClass(),
                    Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
                if (!Target || !PlaceReviewPlayer(**Level, Player, FVector2D(Transform.GetLocation()) + FVector2D(-150, 0)))
                {
                    return Finish(&Player, TEXT("could not place the special-ammo fixture"));
                }
                FWadEncounterProfile Profile;
                Profile.MaxHealth = 10000;
                Profile.RegenPerUpdate = 0;
                Profile.ArmorClass = 2;
                Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
                Target->FinishSpawning(Transform);
                Target->SetActorTickEnabled(false);
                Target->GetCharacterMovement()->DisableMovement();
                AmmoTarget = Target;
                for (int32 Index = 0; Index <= Last; ++Index)
                {
                    Player.CollectAmmo(Index, 1);
                }
                ++NextPoint;
                return true;
            }
            ANativeWadGuardian* Target = AmmoTarget.Get();
            if (!Target)
            {
                return Finish(&Player, TEXT("the special-ammo target disappeared"));
            }
            const int32 Index = (NextPoint - 1) / 2;
            const double Now = Current.GetTimeSeconds();
            if (Index <= Last && (NextPoint % 2) != 0)
            {
                if (Now < StageTime) { return true; }
                if (Player.GetReadyItem() != Crossbow || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, Crossbow);
                    return true;
                }
                Player.SelectAmmo(Index);
                HealthBeforeTravel = Target->GetEncounterHealth();
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Now + 1.5;
                ++NextPoint;
                return true;
            }
            if (Index <= Last)
            {
                if (Now < StageTime) { return true; }
                const auto& Ammo = CrossbowAmmo[Index];
                const int32 Expected = Ammo.Effect == EProjectileDamage::Electricity ? Ammo.Damage * 3 / 2
                    : Ammo.Effect == EProjectileDamage::Piercing ? Ammo.Damage + Ammo.Damage * 8 / 100 : Ammo.Damage;
                if (Player.GetAmmoCount(Index) != 0 || Target->GetEncounterHealth() != HealthBeforeTravel - Expected
                    || (Ammo.Effect == EProjectileDamage::Poison && Target->GetPoisonLevel() != Ammo.Damage / 10))
                {
                    return Finish(&Player, *FString::Printf(TEXT("ammo %s did not apply its damage and spend exactly one round"), Ammo.Name));
                }
                ++NextPoint;
                return true;
            }
            if (NextPoint == UE_ARRAY_COUNT(CrossbowAmmo) * 2 + 1)
            {
                Target->RestorePoison(0, 0.0);
                HealthBeforeTravel = Target->GetEncounterHealth();
                if (!Player.CollectAmmo(Last, 1))
                {
                    return Finish(&Player, TEXT("could not provide the test round for a missed shot"));
                }
                Aim(Player, Player.GetPawnViewLocation() * 2 - Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Now + 2.1;
                ++NextPoint;
                return true;
            }
            if (NextPoint == UE_ARRAY_COUNT(CrossbowAmmo) * 2 + 2)
            {
                if (Now < StageTime) { return true; }
                if (Player.GetAmmoCount(Last) != 0 || Target->GetEncounterHealth() != HealthBeforeTravel)
                {
                    return Finish(&Player, TEXT("a missed shot did not spend exactly one round"));
                }
                ++NextPoint;
                return true;
            }
            if ((NextPoint % 2) != 0)
            {
                HealthBeforeTravel = Target->GetEncounterHealth();
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                return true;
            }
            if (Player.GetInteractionMessage() != TEXT("No bolts.") || Target->GetEncounterHealth() != HealthBeforeTravel)
            {
                return Finish(&Player, TEXT("the empty crossbow could still fire"));
            }
            if (!Player.CollectAmmo(Last, 8) || !Player.SaveWadCheckpoint(Slot))
            {
                return Finish(&Player, TEXT("could not save the continued ammo state"));
            }
            UE_LOG(LogTemp, Display, TEXT("Verified all seven ammo types, armor and poison effects, and missed and empty shots."));
            return Finish(&Player);
        }

        bool TickElymasPresentation(ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
            ANativeWadGuardian* Boss = nullptr;
            for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
            {
                if (Enemy->GetEncounterName() == TEXT("Elymas")) { Boss = *Enemy; break; }
            }
            if (!Level || !Boss || Player.GetHealth() <= 0) { return Finish(&Player, TEXT("the Elymas presentation fixture lost its encounter")); }
            switch (NextPoint)
            {
            case 0:
            {
                if (!bElymasPlatePrimed)
                {
#if WITH_EDITORONLY_DATA
                TArray<uint8> Table;
                const FString Source = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/COLORIZE/COLORT12");
                if (!FFileHelper::LoadFileToArray(Table, *Source) || Table.Num() != 256) { return Finish(&Player, TEXT("Elymas's original palette table is missing")); }
                const TCHAR* Names[] = {TEXT("Elymas"), TEXT("Elymas_Hurt"), TEXT("Elymas_Down"), TEXT("Elymas_Attack")};
                const TCHAR* Art[] = {TEXT("WIZEVL/WIZEVL8.CPC"), TEXT("WIZEVL/WIZHRT1.CPC"), TEXT("WIZEVL/WIZEVL7G.CPC"), TEXT("WIZEVL/WIZEVL1.CPC")};
                for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
                {
                    FIntPoint SpriteSize;
                    TArray<FColor> Pixels;
                    const FString Object = FString::Printf(TEXT("/Game/Maps/L44_Assets/Things/T_%s.T_%s"), Names[Index], Names[Index]);
                    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Object);
                    TArray64<uint8> Actual;
                    if (!Texture || !LoadWadSprite(Art[Index], SpriteSize, Pixels, Table) || !Texture->Source.GetMipData(Actual, 0)
                        || Texture->Source.GetSizeX() != SpriteSize.X || Texture->Source.GetSizeY() != SpriteSize.Y
                        || Actual.Num() != Pixels.Num() * int64(sizeof(FColor))
                        || FMemory::Memcmp(Actual.GetData(), Pixels.GetData(), Actual.Num()) != 0)
                    {
                        return Finish(&Player, *FString::Printf(TEXT("the saved %s sprite differs from the original COLORT12 palette"), Names[Index]));
                    }
                }
                UE_LOG(LogTemp, Display, TEXT("Verified all four saved Elymas sprite pixel buffers match the original art with COLORT12."));
#else
                return Finish(&Player, TEXT("the source palette comparison requires Editor texture data"));
#endif
                for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                {
                    if (*Enemy != Boss) { Enemy->SetActorTickEnabled(false); Enemy->GetCharacterMovement()->SetComponentTickEnabled(false); }
                }
                    if (!PlaceReviewPlayer(**Level, Player, {-268, 1046})) { return Finish(&Player, TEXT("could not reach Elymas's original first pressure plate")); }
                    bElymasPlatePrimed = true;
                    return true;
                }
                FVector2D Plate;
                if (!Level->GetSectorSurfaceHeights(102, Plate) || Plate.X != 512.0) { break; }
                const FVector Origin = Boss->GetActorLocation();
                UE_LOG(LogTemp, Display, TEXT("Elymas presentation source floor: boss %s, capsule half-height %.1f, facing %s."),
                    *Origin.ToCompactString(), Boss->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), *Boss->GetActorForwardVector().ToCompactString());
                const double Floor = Origin.Z - Boss->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                bool bPlaced = false;
                for (const double Distance : {256.0, 288.0, 320.0, 352.0, 384.0, 416.0, 448.0, 512.0, 576.0, 640.0})
                {
                    for (const double Angle : {0.0, -15.0, 15.0, -30.0, 30.0, -45.0, 45.0, -60.0, 60.0, -75.0, 75.0, -85.0, 85.0})
                    {
                        const FVector Candidate = Origin + Boss->GetActorForwardVector().RotateAngleAxis(Angle, FVector::UpVector) * Distance;
                        const int32 Sector = Level->FindWadSector(FVector2D(Candidate));
                        if (Sector != INDEX_NONE && FMath::Abs(Level->GetActorLocation().Z + Level->GetWalkingHeight(Sector) - Floor) < 80.0
                            && PlaceReviewPlayer(**Level, Player, FVector2D(Candidate)) && Boss->CanSeePlayer(Player))
                        {
                            bPlaced = true;
                            break;
                        }
                    }
                    if (bPlaced) { break; }
                }
                if (!bPlaced) { return Finish(&Player, TEXT("could not stage a visible source-floor approach to Elymas")); }
                Aim(Player, Origin);
                ++NextPoint;
                break;
            }
            case 1:
            {
                bool bSaved = false;
                Aim(Player, Boss->GetActorLocation());
                if (!CaptureTownFrame(TEXT("palette"), bSaved)) { break; }
                if (!bSaved) { return Finish(&Player, TEXT("could not capture Elymas's source palette")); }
                ++NextPoint;
                break;
            }
            case 2:
                for (TActorIterator<ANativeWadBolt> Bolt(Player.GetWorld()); Bolt; ++Bolt)
                {
                    if (Bolt->GetOwner() == Boss && !Bolt->IsActorBeingDestroyed())
                    {
                        PresentationBolt = *Bolt;
                        PresentationPreviousPosition = Bolt->GetActorLocation();
                        ++NextPoint;
                        break;
                    }
                }
                break;
            case 3:
                if (ANativeWadBolt* Bolt = PresentationBolt.Get())
                {
                    PresentationPreviousDelta = Bolt->GetActorLocation() - PresentationPreviousPosition;
                    PresentationPreviousPosition = Bolt->GetActorLocation();
                    PresentationPlayerStart = Player.GetActorLocation();
                    SetCombatStrafe(Player, 1);
                    ++NextPoint;
                }
                else { NextPoint = 2; }
                break;
            case 4:
            {
                ANativeWadBolt* Bolt = PresentationBolt.Get();
                if (!Bolt || Bolt->IsActorBeingDestroyed()) { SetCombatStrafe(Player, 0); NextPoint = 2; break; }
                const FVector Delta = Bolt->GetActorLocation() - PresentationPreviousPosition;
                const FVector TowardPlayer = (Player.GetActorLocation() - Bolt->GetActorLocation()).GetSafeNormal();
                const FVector Before = PresentationPreviousDelta.GetSafeNormal();
                const FVector After = Delta.GetSafeNormal();
                if (FVector::Dist2D(Player.GetActorLocation(), PresentationPlayerStart) > 1.0
                    && FVector::DotProduct(Before, After) < 0.999999
                    && FVector::DotProduct(After, TowardPlayer) > FVector::DotProduct(Before, TowardPlayer) + 0.0000001)
                {
                    SetCombatStrafe(Player, 0);
                    Player.GetCharacterMovement()->StopMovementImmediately();
                    UE_LOG(LogTemp, Display, TEXT("Verified Elymas's authored missile bends toward the player during a normal sideways dodge."));
                    ++NextPoint;
                }
                PresentationPreviousDelta = Delta;
                PresentationPreviousPosition = Bolt->GetActorLocation();
                break;
            }
            case 5:
            {
                bool bSaved = false;
                Aim(Player, Boss->GetActorLocation());
                if (!CaptureTownFrame(TEXT("homing"), bSaved)) { break; }
                return Finish(&Player, bSaved ? nullptr : TEXT("could not capture the homing-shot review"));
            }
            }
            return true;
        }

        bool TickBoulderRail(UWorld& Current, ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(&Current);
            ANativeWadGuardian* Rock = nullptr;
            for (TActorIterator<ANativeWadGuardian> Actor(&Current); Actor; ++Actor)
            {
                if (Actor->ActorHasTag(bBoulder75Review ? TEXT("WAD Creature Thing 443") : TEXT("WAD Creature Thing 223")))
                {
                    Rock = *Actor;
                    break;
                }
            }
            if (!Level || !Rock)
            {
                return Finish(&Player, TEXT("the authored boulder is missing"));
            }
            if (bBoulder75Review && NextPoint == 0)
            {
                if (!bCampaignActionStarted)
                {
                    if (!PlaceReviewPlayer(**Level, Player, {2640, 160})) { return Finish(&Player, TEXT("could not stage the L75 boulder-pit approach")); }
                    bCampaignActionStarted = true;
                    return true;
                }
                if (!bBoulderPitTriggered)
                {
                    FWadLevelCheckpoint Pit;
                    if (!Level->WriteCheckpoint(Pit, true)) { return Finish(&Player, TEXT("could not inspect the original boulder-pit trigger")); }
                    if (Pit.Variables[3] == 0)
                    {
                        WalkTo(Player, {2544, 160});
                        return true;
                    }
                    bBoulderPitTriggered = true;
                    UE_LOG(LogTemp, Display, TEXT("Verified normal entry into L75's boulder pit starts the authored lowering and return cycle."));
                }
                Stop(Player);
            }
            const FVector Position = Rock->GetActorLocation();
            if (NextPoint == 0 && (bBoulder75Review ? Position.X > 2600 : Position.X < -3600))
            {
                ++NextPoint;
            }
            else if (NextPoint == 1 && (bBoulder75Review ? Position.X > 2200 && Position.X < 2400 : Position.X > -3000 && Position.X < -2800))
            {
                FWadLevelCheckpoint Checkpoint;
                if (!Level->WriteCheckpoint(Checkpoint, true))
                {
                    return Finish(&Player, TEXT("the boulder position could not be saved"));
                }
                Rock->SetActorLocation(Position + FVector(50, 0, 0));
                if (!Level->RestoreCheckpoint(Checkpoint) || !Position.Equals(Rock->GetActorLocation(), 0.1))
                {
                    return Finish(&Player, TEXT("the boulder position was not restored"));
                }
                if (!PlaceReviewPlayer(**Level, Player, FVector2D(Position)))
                {
                    return Finish(&Player, TEXT("the contact review could not reach the boulder rail"));
                }
                ++NextPoint;
            }
            else if (NextPoint == 2 && Player.GetHealth() == 0)
            {
                UE_LOG(LogTemp, Display, TEXT("Verified the L%d boulder wraps, travels along its source rail, resumes its saved position, and deals contact damage."), bBoulder75Review ? 75 : 73);
                return Finish(&Player);
            }
            return true;
        }

        bool TickAuthoredGenerator(ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
            if (!Level) { return Finish(&Player, TEXT("the authored generator fixture needs its native map")); }
            const bool bReload = Player.GetWorld()->URL.HasOption(TEXT("WadLoad"));
            if (bReload && !Player.HasRestoredWadCheckpoint()) { return true; }
            const int32 Count = bUndeadWizardGeneratorReview ? 4 : 7;
            const int32 Trigger = bUndeadWizardGeneratorReview ? 152 : 264;
            const int32 Creature = bUndeadWizardGeneratorReview ? 1002 : 1006;
            const FVector2D Approach = bUndeadWizardGeneratorReview ? FVector2D(-116, -64) : FVector2D(1344, 3744);
            const FVector2D Plate = bUndeadWizardGeneratorReview ? FVector2D(-116, 40) : FVector2D(1344, 3840);
            FWadLevelCheckpoint State;
            if (!Level->WriteCheckpoint(State, true))
            {
                return Finish(&Player, TEXT("the source creature generators are absent"));
            }
            if (bReload)
            {
                if (NextPoint == 0)
                {
                    for (int32 Id = 0; Id < Count; ++Id)
                    {
                        const FWadCreatureGeneratorState* Generator = State.CreatureGenerators.Find(Id);
                        if (!Generator || Generator->bPaused || Generator->Released != 0 || Generator->Remaining <= 0.0)
                        {
                            return Finish(&Player, TEXT("an active source generator's pending release failed to restore"));
                        }
                    }
                    ++NextPoint;
                }
                if (bUndeadWizardGeneratorReview && NextPoint == 2)
                {
                    if (Player.GetHealth() <= 0) { return Finish(&Player, TEXT("the occupied-spawn fixture lost its player")); }
                    if (State.CreatureGenerators.FindChecked(0).Released == 0) { return true; }
                    if (State.CreatureGenerators.FindChecked(0).Released != 1) { return Finish(&Player, TEXT("the cleared spawn released more than one wizard")); }
                    bool bReleased = false;
                    for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                    {
                        if (Enemy->ActorHasTag(TEXT("WAD Generator 0 Creature 0")) && Enemy->GetSourceType() == 1002
                            && !Enemy->IsHidden() && !Enemy->IsDefeated()) { bReleased = true; }
                    }
                    if (!bReleased) { return Finish(&Player, TEXT("the cleared generator did not release a live Blue Wizard")); }
                    UE_LOG(LogTemp, Display, TEXT("Verified L10's occupied generator retains its release count across restart and releases one wizard after the controlled blocker is cleared."));
                    return Finish(&Player);
                }
                for (int32 Id = 0; Id < Count; ++Id)
                {
                    if (bUndeadWizardGeneratorReview && Id == 0)
                    {
                        if (State.CreatureGenerators.FindChecked(Id).Released != 0) { return Finish(&Player, TEXT("an occupied source spawn advanced its release count")); }
                        continue;
                    }
                    if (State.CreatureGenerators[Id].Released == 0) { return true; }
                    if (State.CreatureGenerators[Id].Released != 1) { return Finish(&Player, TEXT("a restored pending release duplicated")); }
                    const FName Tag(*FString::Printf(TEXT("WAD Generator %d Creature 0"), Id));
                    bool bFound = false;
                    for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                    {
                        if (Enemy->ActorHasTag(Tag) && Enemy->GetSourceType() == Creature && !Enemy->IsHidden() && !Enemy->IsDefeated()) { bFound = true; }
                    }
                    if (!bFound) { return Finish(&Player, TEXT("a restored source generator released the wrong encounter")); }
                }
                if (bUndeadWizardGeneratorReview)
                {
                    const FName BlockerTag(TEXT("WAD Creature Thing 171"));
                    for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                    {
                        if (Enemy->ActorHasTag(BlockerTag) && !Enemy->IsDefeated())
                        {
                            // Controlled collision fixture, not a combat kill or campaign reward.
                            Enemy->RestoreEncounterHealth(0);
                            NextPoint = 2;
                            UE_LOG(LogTemp, Display, TEXT("Verified L10's three unoccupied wizard generators resume once; generator 0 remains blocked at its original occupied spawn."));
                            return true;
                        }
                    }
                    return Finish(&Player, TEXT("the original occupied-spawn blocker is absent"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified %d authored L%d wizard generators resume their pending clock and release exactly one original encounter after process restart."),
                    Count, bUndeadWizardGeneratorReview ? 10 : 41);
                return Finish(&Player);
            }
            if (NextPoint == 0)
            {
                for (int32 Id = 0; Id < Count; ++Id)
                {
                    const FWadCreatureGeneratorState* Inactive = State.CreatureGenerators.Find(Id);
                    if (Inactive && (!Inactive->bPaused || Inactive->Released != 0))
                    {
                        return Finish(&Player, TEXT("the source wizard generators must begin inactive"));
                    }
                }
                if (!PlaceReviewPlayer(**Level, Player, Approach)) { return Finish(&Player, TEXT("could not stage the original generator plate approach")); }
                ++NextPoint;
                return true;
            }
            const FWadCreatureGeneratorState* First = State.CreatureGenerators.Find(0);
            if (!First || First->bPaused)
            {
                if (WalkTo(Player, Plate) && !First) { return Finish(&Player, TEXT("the original plate did not activate its source generators")); }
                return true;
            }
            Stop(Player);
            if (!Player.GetCharacterMovement()->IsMovingOnGround()) { return true; }
            for (int32 Id = 0; Id < Count; ++Id)
            {
                const FWadCreatureGeneratorState* Active = State.CreatureGenerators.Find(Id);
                if (!Active || Active->bPaused || Active->Released != 0)
                {
                    return Finish(&Player, TEXT("the original plate did not activate its complete generator group before release"));
                }
            }
            if (Level->FindWadSector(FVector2D(Player.GetActorLocation())) != Trigger
                || !Player.SaveWadCheckpoint(bUndeadWizardGeneratorReview ? TEXT("NativeWadL10WizardGeneratorReview") : TEXT("NativeWadL41GeneratorReview")))
            {
                return Finish(&Player, TEXT("could not save the grounded source plate with active pending wizard releases"));
            }
            UE_LOG(LogTemp, Display, TEXT("Verified walking onto L%d sector %d activates its %d authored wizard generators and saves their pending release clocks."),
                bUndeadWizardGeneratorReview ? 10 : 41, Trigger, Count);
            return Finish(&Player);
        }

        bool TickRuneOwnership(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            if (HeldKey) { SendKey(Player, HeldKey.GetValue(), IE_Released); HeldKey.Reset(); }
            const bool bReload = Player.GetWorld()->URL.HasOption(TEXT("WadLoad"));
            if (bReload && !Player.HasRestoredWadCheckpoint()) { return true; }
            if (bLegacyRuneReview)
            {
                for (int32 Rune = 1; Rune <= 9; ++Rune)
                {
                    if (Player.HasRune(Rune) != (Rune <= 6) || (Rune <= 6 && Player.GetItemCount(299 + Rune) != 1))
                    {
                        return Finish(&Player, TEXT("legacy rune migration did not install exactly the original Mage starter runes"));
                    }
                }
                if (!bReload && !Player.SaveWadCheckpoint(TEXT("NativeWadRuneLegacyReview")))
                {
                    return Finish(&Player, TEXT("could not save the migrated rune inventory"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified original starter-rune migration without duplicates; process reload=%d."), bReload);
                return Finish(&Player);
            }
            if (bReload)
            {
                if (Player.GetCharacterClass() != MageClass || !Player.HasRune(1) || Player.HasRune(6) || !Player.HasRune(7)
                    || Player.HasRune(8) || Player.HasRune(9) || !Player.HasFeatherfall())
                {
                    return Finish(&Player, TEXT("rune gains, removals, or the spell effect failed to persist"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified acquired and missing runes and Feather Fall across a fresh process restart."));
                return Finish(&Player);
            }
            switch (NextPoint)
            {
            case 0:
            {
                for (int32 Rune = 1; Rune <= 9; ++Rune)
                {
                    if (Player.HasRune(Rune) != (Rune <= 6)) { return Finish(&Player, TEXT("the Mage starter rune ownership differs from the source")); }
                }
                const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
                if (!Catalog) { return Finish(&Player, TEXT("the rune fixture needs the original catalog")); }
                const NativeWadInventoryView::FView Bag(Player, *Catalog);
                for (int32 Slot = 0; Slot < Player.GetCarriedItems().Num(); ++Slot)
                {
                    if (IsRuneItem(Player.GetCarriedItems()[Slot]) && Bag.CellForSlot(Slot) != INDEX_NONE)
                    {
                        return Finish(&Player, TEXT("an installed rune appeared as a selectable bag object"));
                    }
                }
                Player.ConsumeItem(300);
                Press(Player, EKeys::NumPadOne);
                ++NextPoint;
                break;
            }
            case 1:
                if (!Player.GetRunes().IsEmpty() || Player.HasRune(1)) { return Finish(&Player, TEXT("a missing rune accepted its normal key")); }
                Press(Player, EKeys::NumPadFour);
                ++NextPoint;
                break;
            case 2:
                Press(Player, EKeys::NumPadOne);
                ++NextPoint;
                break;
            case 3:
                if (Player.GetRunes() != TEXT("4")) { return Finish(&Player, TEXT("a missing rune completed the spell sequence")); }
                Press(Player, EKeys::BackSpace);
                Player.CollectItem(300);
                Player.CollectItem(305);
                Player.ConsumeItem(305);
                if (!Player.HasRune(6)) { return Finish(&Player, TEXT("removing one duplicate removed the remaining rune")); }
                Player.ConsumeItem(305);
                Player.CollectItem(306);
                Player.CollectItem(317);
                if (Player.HasRune(6) || !Player.HasRune(7) || Player.HasRune(9))
                {
                    return Finish(&Player, TEXT("rune removal, acquisition, or spell-school filtering failed"));
                }
                ++NextPoint;
                break;
            case 4:
                if (Player.HasFeatherfall())
                {
                    if (!Player.SaveWadCheckpoint(TEXT("NativeWadRuneOwnershipReview"))) { return Finish(&Player, TEXT("could not save rune ownership")); }
                    UE_LOG(LogTemp, Display, TEXT("Verified installed rune bag visibility, missing-key refusal, duplicate ownership, school filtering, normal Feather Fall casting, and saving."));
                    return Finish(&Player);
                }
                if (Player.GetMana() <= 250) { return Finish(&Player, TEXT("the normal rune casting retry exhausted mana")); }
                if (Player.GetRunes().IsEmpty()) { Press(Player, EKeys::NumPadFour); }
                else if (Player.GetRunes() == TEXT("4")) { Press(Player, EKeys::NumPadOne); }
                else if (Player.GetRunes() == TEXT("41")) { Press(Player, EKeys::RightMouseButton); }
                break;
            }
            return true;
        }

        bool TickProjectileWands(ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr int32 Types[] = {900, 901, 902, 903, 904, 905, 906, 907, 908, 909, 910, 912, 915, 916, 918};
            if (HeldKey) { SendKey(Player, HeldKey.GetValue(), IE_Released); HeldKey.Reset(); }
            const bool bReload = Player.GetWorld()->URL.HasOption(TEXT("WadLoad"));
            if (bReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                for (int32 Type : Types)
                {
                    if (Player.GetItemCount(Type) != 1) { return Finish(&Player, TEXT("a projectile wand failed to persist")); }
                }
                if (Player.GetReadyItem() != WandOfDisintegration) { return Finish(&Player, TEXT("the last wand's ready hand failed to reload")); }
                UE_LOG(LogTemp, Display, TEXT("Verified eleven damage wands, both Dispel wands, Push, Pull, and the selected ready hand across a process restart."));
                return Finish(&Player);
            }
            const int32 Type = Types[ProjectileWandIndex];
            const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
            const FWadInventoryDetails* Item = Catalog ? Catalog->ItemDetails.Find(Type) : nullptr;
            const FWadWandProjectile* Rules = Item ? FindWandProjectile(Item->WandProjectile) : nullptr;
            if (!Rules || Item->WandManaCost <= 0) { return Finish(&Player, TEXT("a source projectile wand is absent from the catalog")); }
            const double Now = Player.GetWorld()->GetTimeSeconds();
            ANativeWadGuardian* Target = ElementalTarget.Get();
            switch (NextPoint)
            {
            case 0:
            {
                const TActorIterator<ANativeWadLevel> Level(Player.GetWorld());
                const TActorIterator<ANativeWadGuardian> Original(Player.GetWorld());
                if (!Level || !Original) { return Finish(&Player, TEXT("the projectile fixture needs L30's encounter room")); }
                const FVector Position = Original->GetActorLocation();
                for (TActorIterator<ANativeWadGuardian> Enemy(Player.GetWorld()); Enemy; ++Enemy)
                {
                    Enemy->SetActorTickEnabled(false);
                    Enemy->SetActorEnableCollision(false);
                    Enemy->SetActorHiddenInGame(true);
                }
                if (!PlaceReviewPlayer(**Level, Player, FVector2D(Position) + FVector2D(-150, 0)))
                {
                    return Finish(&Player, TEXT("the projectile fixture could not use the original room"));
                }
                const FTransform Transform(FRotator::ZeroRotator, Position);
                Target = Player.GetWorld()->SpawnActorDeferred<ANativeWadGuardian>(ANativeWadGuardian::StaticClass(), Transform,
                    nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
                FWadEncounterProfile Profile;
                Profile.Name = TEXT("Controlled projectile-wand target");
                Profile.MaxHealth = 10000;
                Profile.RegenPerUpdate = 0;
                Profile.WalkSpeed = 0;
                Target->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
                UGameplayStatics::FinishSpawningActor(Target, Transform);
                Target->SetActorTickEnabled(false);
                ElementalTarget = Target;
                ++NextPoint;
                break;
            }
            case 1:
                if (Now < StageTime) { break; }
                Target->RestoreEncounterHealth(10000);
                Target->RestorePoison(0, 0);
                Target->RestoreControlEffects(Type == 909 || Type == 910 ? 60 : 0, 0);
                Player.RestoreWadVital(9, Player.GetMaxHealth());
                Player.RestoreWadVital(10, 2 * Item->WandManaCost - Player.GetMana());
                if (Player.GetItemCount(Type) == 0) { Player.CollectItem(Type); }
                ++NextPoint;
                break;
            case 2:
                if (Player.GetReadyItem() != Type || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, Type); break; }
                ProjectileTargetStart = Target->GetActorLocation();
                HealthBeforeCast = Player.GetHealth();
                ExperienceBeforeTravel = Player.GetExperience();
                Aim(Player, Target->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                StageTime = Now + 0.5;
                ++NextPoint;
                break;
            case 3:
            {
                if (Now < StageTime) { break; }
                const int32 Expected = (Rules->DamageMask & SpecialDamage) ? 0
                    : Rules->Damage + ((Rules->DamageMask & DamageFlag(EProjectileDamage::Fire)) ? Rules->Damage / 4 : 0);
                if (Target->GetEncounterHealth() != 10000 - Expected || Player.GetExperience() != ExperienceBeforeTravel + Expected
                    || Player.GetMana() != Item->WandManaCost || Player.GetItemCount(Type) != 1)
                {
                    return Finish(&Player, *FString::Printf(TEXT("wand %d hit mismatch: target=%d mana=%d"), Type, Target->GetEncounterHealth(), Player.GetMana()));
                }
                if (Target->GetPoisonLevel() != (!(Rules->DamageMask & SpecialDamage)
                    && (Rules->DamageMask & DamageFlag(EProjectileDamage::Poison)) ? Rules->Damage / 10 : 0))
                {
                    return Finish(&Player, TEXT("a wand applied the wrong poison component"));
                }
                if (Rules->DamageMask == SpecialDamageFlag(ESpecialDamage::Push) || Rules->DamageMask == SpecialDamageFlag(ESpecialDamage::Pull))
                {
                    const FVector Away = (ProjectileTargetStart - Player.GetActorLocation()).GetSafeNormal2D();
                    const double Travel = FVector::DotProduct(Target->GetActorLocation() - ProjectileTargetStart, Away);
                    if (Rules->DamageMask == SpecialDamageFlag(ESpecialDamage::Push) ? Travel <= 0.1 : Travel >= -0.1)
                    { return Finish(&Player, *FString::Printf(TEXT("force wand %d moved the live target %.3f units in the wrong direction"), Type, Travel)); }
                    UE_LOG(LogTemp, Display, TEXT("Verified force wand %d moves the live target %.3f units without health damage or XP."), Type, Travel);
                }
                if ((Type == 909 || Type == 910) && Target->GetSlowSeconds() <= 0.0)
                { return Finish(&Player, TEXT("Dispel removed a creature effect, contrary to the source")); }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            }
            case 4:
                if (Player.GetMana() != Item->WandManaCost / 2 || Player.GetItemCount(Type) != 1)
                {
                    return Finish(&Player, TEXT("a projectile wand failed its exact-cost mana refusal"));
                }
                for (TActorIterator<ANativeWadBolt> Bolt(Player.GetWorld()); Bolt; ++Bolt)
                {
                    if (Bolt->GetOwner() == &Player && !Bolt->IsActorBeingDestroyed()) { return Finish(&Player, TEXT("a refused wand shot spawned a missile")); }
                }
                UE_LOG(LogTemp, Display, TEXT("Verified original wand %d projectile %d: damage, elements, mana, refusal, and retained item."), Type, Rules->Type);
                if (++ProjectileWandIndex < UE_ARRAY_COUNT(Types)) { StageTime = Now + 0.25; NextPoint = 1; break; }
                if (!Player.SaveWadCheckpoint(TEXT("NativeWadProjectileWandsReview"))) { return Finish(&Player, TEXT("could not save the projectile-wand fixture")); }
                return Finish(&Player);
            }
            return true;
        }

        bool TickWandCombat(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            const int32 Wand = bFireballWandReview ? 903 : WandOfDisintegration;
            const int32 Cost = bFireballWandReview ? 800 : DisintegrationManaCost;
            const TActorIterator<ANativeWadLevel> Level(&Current);
            const TActorIterator<ANativeWadGuardian> Guardian(&Current);
            if (!Level || !Guardian || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the wand review lost its player or target"));
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const double Now = Current.GetTimeSeconds();
            switch (NextPoint)
            {
            case 0:
            {
                const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
                const FWadInventoryDetails* Details = Catalog ? Catalog->ItemDetails.Find(Wand) : nullptr;
                if (!Catalog || (bFireballWandReview
                    ? !Catalog->WandProjectileMaterials.FindRef(4005) || !Details || Details->WandManaCost != Cost
                    : !Catalog->PlasmaProjectile))
                {
                    return Finish(&Player, TEXT("the wand projectile material is missing from the item catalog"));
                }
                if (!Player.CollectItem(Wand))
                {
                    return Finish(&Player, TEXT("could not supply the wand for the ranged test"));
                }
                ++NextPoint;
                break;
            }
            case 1:
                if (Player.GetReadyItem() != Wand || Player.IsInventoryOpen())
                {
                    ReadyCampaignItem(Player, Wand);
                    break;
                }
                if (Player.GetMana() != 2 * Cost)
                {
                    return Finish(&Player, *FString::Printf(TEXT("start the wand review with WadMana=%d"), Cost * 2));
                }
                if (!PlaceReviewPlayer(**Level, Player, FVector2D(Guardian->GetActorLocation()) + FVector2D(-150, 0)))
                {
                    return Finish(&Player, TEXT("could not set up the wand shot"));
                }
                HealthBeforeTravel = Guardian->GetEncounterHealth();
                HealthBeforeCast = Player.GetHealth();
                ExperienceBeforeTravel = Player.GetExperience();
                StageTime = Now;
                Aim(Player, Guardian->GetActorLocation());
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 2:
                if (Player.GetMana() != Cost || Player.GetItemCount(Wand) != 1)
                {
                    return Finish(&Player, TEXT("the shot spent the wrong mana or consumed the wand"));
                }
                if (Now - StageTime < 0.3) { break; }
                if (!Guardian->IsDefeated() || Player.GetExperience() != ExperienceBeforeTravel + HealthBeforeTravel)
                {
                    return Finish(&Player, TEXT("the wand missed the Guardian or awarded the wrong experience"));
                }
                if (Player.GetHealth() != HealthBeforeCast - (bFireballWandReview ? 0 : ResolvePlayerDamage(PlasmaDamage, PlasmaElements, Player.GetResistanceMask(), 0, Player.GetArmorRating()).Health))
                {
                    return Finish(&Player, TEXT("the caster received the wrong splash damage for the wand's radius"));
                }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 3:
                if (Player.GetMana() != Cost / 2 || Player.GetItemCount(Wand) != 1)
                {
                    return Finish(&Player, TEXT("the wand failed to apply the half-cost penalty at exactly its mana cost"));
                }
                for (TActorIterator<ANativeWadBolt> Bolt(&Current); Bolt; ++Bolt)
                {
                    if (Bolt->GetOwner() == &Player && !Bolt->IsActorBeingDestroyed())
                    {
                        return Finish(&Player, TEXT("the wand spawned a projectile with insufficient mana"));
                    }
                }
                UE_LOG(LogTemp, Display, TEXT("Verified %s ranged hit, splash radius, mana costs, and retained item."),
                    *Player.GetInventoryItemDescription(Wand));
                return Finish(&Player);
            }
            return true;
        }

        bool TickRangedAndTimed(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            constexpr const TCHAR* TimedSlot = TEXT("NativeWadTimedItemsReview");
            const TActorIterator<ANativeWadLevel> Level(&Current);
            const TActorIterator<ANativeWadGuardian> Guardian(&Current);
            if (!Level || !Guardian || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the combat review lost its player or target"));
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            if (bTimedItemReload)
            {
                if (!Player.HasRestoredWadCheckpoint()) { return true; }
                const double Now = Current.GetTimeSeconds();
                if (NextPoint == 0)
                {
                    if (!Player.HasFeatherfall() || Player.GetArmorRating() != 10 || !Player.IsWaterWalking())
                    {
                        return Finish(&Player, TEXT("the timed effects did not survive a process restart"));
                    }
                    StageTime = Now;
                    ++NextPoint;
                }
                if (Now - StageTime < ShieldPotionSeconds + 1.0) { return true; }
                if (Player.HasFeatherfall() || Player.GetArmorRating() != 0 || Player.IsWaterWalking())
                {
                    return Finish(&Player, TEXT("the featherfall, shield, or water walk potion failed to expire"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the timed shield, featherfall, and water walk effects persist across restart and expire."));
                return Finish(&Player);
            }
            switch (NextPoint)
            {
            case 0:
                Player.CollectItem(Crossbow);
                Player.CollectItem(NormalQuiver);
                Player.CollectItem(ShieldPotionFirst);
                Player.CollectItem(FeatherfallPotion);
                Player.CollectItem(WaterWalkPotion);
                ExperienceBeforeTravel = Player.GetExperience();
                HealthBeforeTravel = Guardian->GetEncounterHealth();
                ++NextPoint;
                break;
            case 1:
                if (Player.GetReadyItem() != ShieldPotionFirst || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, ShieldPotionFirst); break; }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 2:
                if (Player.GetItemCount(ShieldPotionFirst) || Player.GetArmorRating() != 10)
                {
                    return Finish(&Player, TEXT("the timed shield potion did not apply ten armor points"));
                }
                if (Player.GetReadyItem() != FeatherfallPotion || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, FeatherfallPotion); break; }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 3:
                if (Player.GetItemCount(FeatherfallPotion) || !Player.HasFeatherfall())
                {
                    return Finish(&Player, TEXT("the featherfall potion did not activate"));
                }
                if (Player.GetReadyItem() != WaterWalkPotion || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, WaterWalkPotion); break; }
                Press(Player, EKeys::LeftControl);
                ++NextPoint;
                break;
            case 4:
                if (Player.GetItemCount(WaterWalkPotion) || !Player.IsWaterWalking())
                {
                    return Finish(&Player, TEXT("the timed water walk potion did not activate"));
                }
                if (!Player.GetCharacterMovement()->IsMovingOnGround()) { break; }
                if (!Player.SaveWadCheckpoint(TimedSlot))
                {
                    return Finish(&Player, TEXT("could not save the active effects at the L30 spawn"));
                }
                Player.SetActorLocation(Player.GetActorLocation() + FVector(0, 0, 35), true);
                Player.GetCharacterMovement()->SetMovementMode(MOVE_Falling);
                Player.GetCharacterMovement()->Velocity.Z = -300.0;
                ++NextPoint;
                break;
            case 5:
                if (!Player.GetCharacterMovement()->IsFalling()) { break; }
                if (!FMath::IsNearlyEqual(Player.GetVelocity().Z, -FeatherfallSpeed, 1.0))
                {
                    return Finish(&Player, TEXT("featherfall did not cap the player's fall speed"));
                }
                ++NextPoint;
                break;
            case 6:
                if (Player.GetReadyItem() != Crossbow || Player.IsInventoryOpen()) { ReadyCampaignItem(Player, Crossbow); break; }
                {
                    const FVector Target = Guardian->GetActorLocation();
                    if (!PlaceReviewPlayer(**Level, Player, FVector2D(Target) + FVector2D(-150, 0)))
                    {
                        return Finish(&Player, TEXT("could not set up an outside-melee-range crossbow shot"));
                    }
                    Aim(Player, Target);
                    Press(Player, EKeys::LeftControl);
                    ++NextPoint;
                }
                break;
            case 7:
                if (Player.GetItemCount(NormalQuiver) || Player.GetItemCount(NormalAmmoBolt) != NormalQuiverBolts - 1)
                {
                    return Finish(&Player, TEXT("the crossbow did not consume one round from the quiver"));
                }
                if (Guardian->GetEncounterHealth() != HealthBeforeTravel - CrossbowDamage) { break; }
                if (Player.GetExperience() != ExperienceBeforeTravel + CrossbowDamage)
                {
                    return Finish(&Player, TEXT("projectile damage did not award combat experience"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the ranged hit, one normal bolt spent, and the shield, featherfall, and water walk potions."));
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
            if (Now - StartTime > (bFreshCampaign || CampaignRoute || CampaignLaunch ? 900.0
                : bRiverReview || bRavineReview || TownReview == ETownReview::Tour || TownReview == ETownReview::Failure
                ? 270.0 : bSlice ? 180.0 : 120.0))
            {
                return Finish(Player, TEXT("the review timed out"));
            }
            if (!Player)
            {
                return true;
            }
            if (bEconomyReview)
            {
                return TickEconomy(*Current, *Player);
            }
            if (bRangedReview)
            {
                return TickRangedAndTimed(*Current, *Player);
            }
            if (bAuthoredGeneratorReview) { return TickAuthoredGenerator(*Player); }
            if (bRuneOwnershipReview) { return TickRuneOwnership(*Player); }
            if (bProjectileWandsReview) { return TickProjectileWands(*Player); }
            if (bWandReview)
            {
                return TickWandCombat(*Current, *Player);
            }
            if (bElymasPresentationReview) { return TickElymasPresentation(*Player); }
            if (bBoulderReview)
            {
                return TickBoulderRail(*Current, *Player);
            }
            if (bAmmoReview)
            {
                return TickSpecialAmmo(*Current, *Player);
            }
            if (bAmmoBannerReview)
            {
                return TickAmmoBanner(*Current, *Player);
            }
            if (bDispelReview) { return TickDispelEffects(*Player); }
            if (bControlWandReview) { return TickControlWands(*Player); }
            if (bRuneProjectileReview) { return TickRuneProjectiles(*Player); }
            if (bConfusionWandReview) { return TickConfusionWand(*Player); }
            if (bResistanceReview) { return TickResistanceItems(*Player); }
            if (bUtilityReview)
            {
                return TickUtilityItems(*Current, *Player);
            }
            if (bReviveReview)
            {
                return TickRevive(*Current, *Player);
            }
            if (bMusicReview)
            {
                return TickMusicTravel(*Current, *Player);
            }
            if (bLavaTerrainReview) { return TickLavaTerrain(*Player); }
            if (bInfiniteGeneratorReview) { return TickInfiniteGenerator(*Player); }
            if (bElementalMeleeReview) { return TickElementalMelee(*Player); }
            if (bPendingCharacterReview) { return TickPendingCharacter(*Player); }
            if (bClassEquipmentReview)
            {
                return TickClassEquipment(*Player);
            }
            if (bCharacterReview)
            {
                return TickCharacter(*Current, *Player);
            }
            if (TownReview == ETownReview::Inn || TownReview == ETownReview::InnReload)
            {
                return TickInn(*Current, *Player);
            }
            if (bClassicHudReview)
            {
                return TickClassicHud(*Player);
            }
            if (bInventoryReview)
            {
                return TickInventoryReview(*Player);
            }
            if (bItemUseReview)
            {
                return TickItemUseReview(*Player);
            }
            if (TownReview != ETownReview::None)
            {
                return TickTown(*Current, *Player);
            }
            if (UndeadReview != EUndeadReview::None)
            {
                return TickUndead(*Current, *Player);
            }
            if (TempleReview != ETempleReview::None)
            {
                return TickTemple(*Current, *Player);
            }
            if (CampaignRoute || CampaignLaunch)
            {
                return TickCampaign(*Current, *Player);
            }
            if (L25Review != EL25Review::None)
            {
                return TickL25(*Current, *Player);
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
                    if (bSlice && (Player->GetItemCount(NativeWadRules::Berry) != SliceStartingBerries
                        || Player->GetItemCount(NativeWadRules::IronDagger) != 1
                        || Player->GetEquippedWeapon() != (Player->CanUseWadItem(NativeWadRules::IronDagger) ? NativeWadRules::IronDagger : NativeWadRules::IronMace) || Player->GetSuccessfulCasts() != 1
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
                        UE_LOG(LogTemp, Display, TEXT("Completed continuous L30 gameplay slice. L301 arrival: level %d, health %d/%d, mana %d/%d, food %d, XP %d/%d, starter weapon ready."),
                            Player->GetCharacterLevel(), Player->GetHealth(), Player->GetMaxHealth(), Player->GetMana(), Player->GetMaxMana(),
                            Player->GetFood(), Player->GetExperience(), Player->GetExperienceNeeded());
                    }
                }
                if (bFreshCampaign)
                {
                    BeginCampaignMap(*Current, *Player);
                    bSlice = false;
                    bCampaign = false;
                    bRiverReview = true;
                    Stage = EStage::Spawn;
                    return true;
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
    const bool bWholeCampaign = Mode == TEXT("freshcampaignchain");
    const bool bFreshCampaign = Mode == TEXT("freshcampaign") || bWholeCampaign;
    const bool bCampaignChain = bWholeCampaign || (Mode.StartsWith(TEXT("campaign")) && Mode.EndsWith(TEXT("chain")));
    const bool bCampaignMovementSample = Mode == TEXT("campaignl20timing");
    const FString CampaignMode = bCampaignMovementSample ? TEXT("campaignl20") : bCampaignChain ? Mode.LeftChop(5) : Mode;
    const auto* CampaignRoute = Algo::FindByPredicate(CampaignRoutes,
        [&CampaignMode](const FCampaignRoute& Route)
        {
            return !Route.Actions.IsEmpty() && CampaignMode == FString::Printf(TEXT("campaignl%d"), Route.Map);
        });
    const auto* CampaignLaunch = Algo::FindByPredicate(CampaignQuests,
        [&CampaignMode](const FCampaignTurnIn& Quest) { return CampaignMode == FString::Printf(TEXT("campaigntown%d"), Quest.Quest); });
    const ETempleReview TempleReview = Mode == TEXT("templelaunch") ? ETempleReview::Launch
        : Mode == TEXT("templel51") ? ETempleReview::L51 : Mode == TEXT("templel52") ? ETempleReview::L52
        : Mode == TEXT("templel52reload") ? ETempleReview::Reload : ETempleReview::None;
    const EUndeadReview UndeadReview = Mode == TEXT("undeadlaunch") ? EUndeadReview::Launch
        : Mode == TEXT("undeadcastle") ? EUndeadReview::Castle : Mode == TEXT("undeadtraps") ? EUndeadReview::Traps
        : Mode == TEXT("undeadkeep") ? EUndeadReview::Keep : Mode == TEXT("undeadboss") ? EUndeadReview::Boss
        : Mode == TEXT("undeadreturn") ? EUndeadReview::Return : Mode == TEXT("undeadempty") ? EUndeadReview::Empty : EUndeadReview::None;
    const EL25Review L25Review = Mode == TEXT("churchpassages") ? EL25Review::Passages
        : Mode == TEXT("churchkeyreload") ? EL25Review::KeyReload
        : Mode == TEXT("churchdoorreload") ? EL25Review::DoorReload : Mode == TEXT("churchknights") ? EL25Review::Knights
        : Mode == TEXT("churchjourney") ? EL25Review::Exit : Mode == TEXT("churchl20reload") ? EL25Review::L20Reload
        : Mode == TEXT("churchscenery") ? EL25Review::Scenery : Mode == TEXT("churchl20scenery") ? EL25Review::L20Scenery
        : Mode == TEXT("churchl20exit") ? EL25Review::L20Exit : Mode == TEXT("churchl60reload") ? EL25Review::L60Reload
        : Mode == TEXT("churchl60puzzles") ? EL25Review::L60Puzzles : Mode == TEXT("churchl60relic") ? EL25Review::L60Relic
        : Mode == TEXT("churchl60return") ? EL25Review::L60Return : Mode == TEXT("churchl60empty") ? EL25Review::L60Empty
        : Mode == TEXT("elmorelaunch") ? EL25Review::ElmoreLaunch : Mode == TEXT("elmorel64") ? EL25Review::ElmoreL64
        : Mode == TEXT("elmorel64reload") ? EL25Review::ElmoreL64Reload : Mode == TEXT("elmorel65") ? EL25Review::ElmoreL65
        : Mode == TEXT("elmorel65reload") ? EL25Review::ElmoreL65Reload
        : Mode == TEXT("elmorecombat64") ? EL25Review::ElmoreCombat64 : Mode == TEXT("elmorecombat65") ? EL25Review::ElmoreCombat65
        : Mode == TEXT("elmorel65exit") ? EL25Review::ElmoreL65Exit : Mode == TEXT("elmorel34") ? EL25Review::ElmoreL34
        : Mode == TEXT("elmorel151reload") ? EL25Review::ElmoreL151Reload
        : Mode == TEXT("elmorel151exit") ? EL25Review::ElmoreL151Exit : Mode == TEXT("elmorel152") ? EL25Review::ElmoreL152
        : Mode == TEXT("elmorel153puzzles") ? EL25Review::ElmoreL153Puzzles : Mode == TEXT("elmoreorb") ? EL25Review::ElmoreOrb
        : Mode == TEXT("elmoreorbreturn") ? EL25Review::ElmoreOrbReturn : Mode == TEXT("elmoreorbempty") ? EL25Review::ElmoreOrbEmpty
        : EL25Review::None;
    const ETownReview TownReview = Mode == TEXT("towntour") ? ETownReview::Tour : Mode == TEXT("townfailure") ? ETownReview::Failure
        : Mode == TEXT("townreload") || L25Review == EL25Review::Exit ? ETownReview::Reload
        : Mode == TEXT("townstart") || bFreshCampaign ? ETownReview::Start
        : Mode == TEXT("townscroll") ? ETownReview::Scroll
        : Mode == TEXT("townservices") ? ETownReview::Services : Mode == TEXT("townservicesreload") ? ETownReview::ServicesReload
        : Mode == TEXT("townservicesloadout") ? ETownReview::ServicesLoadout
        : Mode == TEXT("towninn") ? ETownReview::Inn : Mode == TEXT("towninnreload") ? ETownReview::InnReload
        : ETownReview::None;
    const bool bStartsInTown = TownReview == ETownReview::Reload || TownReview == ETownReview::Start
        || TownReview == ETownReview::Scroll || TownReview == ETownReview::Services || TownReview == ETownReview::ServicesReload
        || TownReview == ETownReview::ServicesLoadout
        || TownReview == ETownReview::Inn || TownReview == ETownReview::InnReload
        || L25Review == EL25Review::ElmoreLaunch
        || UndeadReview == EUndeadReview::Launch || TempleReview == ETempleReview::Launch || CampaignLaunch
        || Mode == TEXT("characterpath") || Mode == TEXT("pendingcharacter") || (Mode == TEXT("classequipment") && World && !World->URL.HasOption(TEXT("WadLoad")));
    const bool bHasReviewWorld = bStartsInTown ? World && World->IsGameWorld() && World->GetAuthGameMode<ANativeWadTownGameMode>()
        : GetReviewLevel(World, OutputPath, ImageSize) != nullptr;
    const FWorldContext* Context = bHasReviewWorld ? GEngine->GetWorldContextFromWorld(World) : nullptr;
    const bool bAmbient = Mode == TEXT("ambient");
    const bool bClassicHud = Mode == TEXT("classichud");
    const bool bInventory = Mode == TEXT("inventory") || Mode == TEXT("inventoryreload");
    const bool bItemUse = Mode == TEXT("itemuse") || Mode == TEXT("itemusereload");
    const bool bEconomy = Mode == TEXT("economywater") || Mode == TEXT("economywaterreload");
    const bool bEconomyReload = Mode == TEXT("economywaterreload");
    const bool bRanged = Mode == TEXT("rangedandtimed") || Mode == TEXT("timeditemreload");
    const bool bWand = Mode == TEXT("wandcombat") || Mode == TEXT("wandfireball");
    const bool bBoulder = Mode == TEXT("boulderrail") || Mode == TEXT("boulderrail75");
    const bool bAmmo = Mode == TEXT("specialammo") || Mode == TEXT("specialammoreload");
    const bool bAmmoBanner = Mode == TEXT("ammobanner");
    const bool bDispel = Mode == TEXT("dispeleffects") || Mode == TEXT("dispeleffectsreload");
    const bool bControlWands = Mode == TEXT("controlwands") || Mode == TEXT("controlwandsreload");
    const bool bRuneProjectile = Mode == TEXT("runeprojectiles") || Mode == TEXT("runeprojectilesreload");
    const bool bConfusion = Mode == TEXT("confusionwand") || Mode == TEXT("confusionwandreload");
    const bool bResistance = Mode == TEXT("resistanceitems") || Mode == TEXT("resistanceitemsreload");
    const bool bUtility = Mode == TEXT("utilityitems") || Mode == TEXT("utilityitemsreload");
    const bool bRevive = Mode == TEXT("revive") || Mode == TEXT("revivereload");
    const bool bMusic = Mode == TEXT("musictravel");
    const bool bCharacter = Mode == TEXT("characterpath") || Mode == TEXT("characterreload");
    const bool bL22Journey = Mode == TEXT("l31tol22");
    const bool bL22Trap = Mode == TEXT("l22trap");
    const bool bLiftCheckpoint = Mode == TEXT("liftsave") || Mode == TEXT("liftreload");
    const bool bDoorReload = Mode == TEXT("doorreload");
    const bool bStartsInL20 = bAmmoBanner || L25Review == EL25Review::L20Reload || L25Review == EL25Review::L20Scenery
        || L25Review == EL25Review::L20Exit;
    const FString CampaignMap = CampaignRoute ? FString::Printf(TEXT("L%d"), CampaignRoute->Map) : FString();
    const TCHAR* StartMap = bStartsInTown ? TEXT("WadTown") : CampaignRoute ? *CampaignMap : bStartsInL20 ? TEXT("L20")
        : bControlWands ? TEXT("L61")
        : Mode == TEXT("elymaspresentation") ? TEXT("L44") : Mode == TEXT("templegenerator") ? TEXT("L41") : Mode == TEXT("undeadwizardgenerator") ? TEXT("L10")
        : (Mode == TEXT("infinitegenerator") || Mode == TEXT("lavaterrain")) ? TEXT("L71")
        : TempleReview == ETempleReview::L51 ? TEXT("L51") : TempleReview != ETempleReview::None ? TEXT("L52")
        : UndeadReview == EUndeadReview::Castle ? TEXT("L231") : UndeadReview == EUndeadReview::Traps ? TEXT("L75")
        : UndeadReview == EUndeadReview::Keep ? TEXT("L10") : UndeadReview != EUndeadReview::None ? TEXT("L12")
        : L25Review >= EL25Review::ElmoreL153Puzzles ? TEXT("L153") : L25Review == EL25Review::ElmoreL152 ? TEXT("L152")
        : L25Review == EL25Review::ElmoreL151Exit ? TEXT("L151")
        : L25Review == EL25Review::ElmoreL151Reload ? TEXT("L151") : L25Review == EL25Review::ElmoreL34 ? TEXT("L34")
        : L25Review == EL25Review::ElmoreCombat64 ? TEXT("L64")
        : L25Review >= EL25Review::ElmoreL65 ? TEXT("L65")
        : L25Review >= EL25Review::ElmoreL64 ? TEXT("L64")
        : L25Review >= EL25Review::L60Reload ? TEXT("L60")
        : L25Review != EL25Review::None ? TEXT("L25") : bAmbient || bClassicHud ? TEXT("L301")
        : bEconomyReload ? TEXT("L301")
        : bBoulder ? (Mode == TEXT("boulderrail75") ? TEXT("L75") : TEXT("L73"))
        : bInventory || bItemUse || bEconomy || bRanged || bWand || bAmmo || bDispel || bRuneProjectile || bConfusion || bResistance || bUtility || bRevive || bMusic || bCharacter
            || Mode == TEXT("classequipment") || Mode == TEXT("elementalmelee") || Mode == TEXT("projectilewands") || Mode == TEXT("runeownership") || Mode == TEXT("runelegacy") ? TEXT("L30")
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
    Review->bClassicHudReview = bClassicHud;
    Review->bEconomyReview = bEconomy;
    Review->bEconomyReload = bEconomyReload;
    Review->bRangedReview = bRanged;
    Review->bTimedItemReload = Mode == TEXT("timeditemreload");
    Review->bWandReview = bWand;
    Review->bFireballWandReview = Mode == TEXT("wandfireball");
    Review->bBoulderReview = bBoulder;
    Review->bBoulder75Review = Mode == TEXT("boulderrail75");
    Review->bElymasPresentationReview = Mode == TEXT("elymaspresentation");
    Review->bAmmoReview = bAmmo;
    Review->bAmmoBannerReview = bAmmoBanner;
    Review->bDispelReview = bDispel;
    Review->bDispelReload = Mode == TEXT("dispeleffectsreload");
    Review->bControlWandReview = bControlWands;
    Review->bControlWandReload = Mode == TEXT("controlwandsreload");
    Review->bRuneProjectileReview = bRuneProjectile;
    Review->bRuneProjectileReload = Mode == TEXT("runeprojectilesreload");
    Review->bConfusionWandReview = bConfusion;
    Review->bConfusionWandReload = Mode == TEXT("confusionwandreload");
    Review->bResistanceReview = bResistance;
    Review->bResistanceReload = Mode == TEXT("resistanceitemsreload");
    Review->bUtilityReview = bUtility;
    Review->bUtilityReload = Mode == TEXT("utilityitemsreload");
    Review->bAmmoReload = Mode == TEXT("specialammoreload");
    Review->bReviveReview = bRevive;
    Review->bReviveReload = Mode == TEXT("revivereload");
    Review->bMusicReview = bMusic;
    Review->bCharacterReview = bCharacter;
    Review->bClassEquipmentReview = Mode == TEXT("classequipment");
    Review->bPendingCharacterReview = Mode == TEXT("pendingcharacter");
    Review->bElementalMeleeReview = Mode == TEXT("elementalmelee");
    Review->bProjectileWandsReview = Mode == TEXT("projectilewands");
    Review->bRuneOwnershipReview = Mode == TEXT("runeownership") || Mode == TEXT("runelegacy");
    Review->bLegacyRuneReview = Mode == TEXT("runelegacy");
    Review->bAuthoredGeneratorReview = Mode == TEXT("templegenerator") || Mode == TEXT("undeadwizardgenerator");
    Review->bUndeadWizardGeneratorReview = Mode == TEXT("undeadwizardgenerator");
    Review->bInfiniteGeneratorReview = Mode == TEXT("infinitegenerator");
    Review->bLavaTerrainReview = Mode == TEXT("lavaterrain");
    Review->bCharacterReload = Mode == TEXT("characterreload");
    Review->bInventoryReview = bInventory;
    Review->bInventoryReload = Mode == TEXT("inventoryreload");
    Review->bItemUseReview = bItemUse;
    Review->bItemUseReload = Mode == TEXT("itemusereload");
    Review->bRavineReview = Mode == TEXT("ravine") || bL22Journey;
    Review->bL22Journey = bL22Journey;
    Review->bL22TrapReview = bL22Trap;
    Review->bLiftCheckpointReview = bLiftCheckpoint;
    Review->bLiftReload = Mode == TEXT("liftreload");
    Review->bDoorReload = bDoorReload;
    Review->TownReview = TownReview;
    Review->UndeadReview = UndeadReview;
    Review->TempleReview = TempleReview;
    Review->CampaignRoute = CampaignRoute;
    Review->CampaignLaunch = CampaignLaunch;
    Review->bCampaignChain = bCampaignChain;
    Review->bCampaignMovementSample = bCampaignMovementSample;
    Review->bFreshCampaign = bFreshCampaign;
    Review->bWholeCampaign = bWholeCampaign;
    Review->L25Review = L25Review;
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
