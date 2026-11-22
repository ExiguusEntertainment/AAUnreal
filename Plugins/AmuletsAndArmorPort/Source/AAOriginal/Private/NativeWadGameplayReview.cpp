#include "CoreMinimal.h"
#include "NativeWadReview.h"

#include "Algo/Find.h"
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
#include "Misc/Paths.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "UnrealClient.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadScenery.h"
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

    enum class ETownReview { None, Tour, Failure, Reload, Start, Scroll };
    enum class EUndeadReview { None, Launch, Castle, Traps, Keep, Boss, Return, Empty };
    enum class ETempleReview { None, Launch, L51, L52, Reload };
    enum class ECampaignAction { Place, Walk, Jump, Use, Floor, Ceiling, OpenDoor, HealthAtLeast, Pickup, Defeat, Frame,
        Rejuvenate, ManaAtLeast, StatueAngle, Checkpoint, Teleport };

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

    constexpr int32 GreaterHealingPotion = 801;
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

    const FCampaignAction Campaign52[] = {
        {ECampaignAction::Walk, {-3448, 1440, 0}},
    };
    const FCampaignAction Campaign41[] = {
        {ECampaignAction::Place, {3552, -2018, 0}}, {ECampaignAction::Use, {3552, -2048, 42}},
        {ECampaignAction::Ceiling, {0, 0, 130}, 220, TEXT("L41 timed switch raises gate 220 to Z130")},
        {ECampaignAction::Place, {-76, 2942, 0}}, {ECampaignAction::Pickup, {-76, 2910, 52}, NativeWadRules::ElmoreKey6},
        {ECampaignAction::Place, {3621, 832, 0}}, {ECampaignAction::Use, {3677, 832, 40}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 37, TEXT("L41 Key 6 opens door 37")},
        {ECampaignAction::Frame, {3677, 832, 44}, INDEX_NONE, TEXT("key-gate")},
        {ECampaignAction::Walk, {3740, 832, 0}},
        {ECampaignAction::Place, {1344, 6724, 0}}, {ECampaignAction::Walk, {1344, 6782, 0}},
    };
    const FCampaignAction Campaign44[] = {
        {ECampaignAction::Rejuvenate, {0, 0, 2000}, INDEX_NONE, TEXT("the carried character recovers before facing Elymas")},
        {ECampaignAction::Place, {-714, -414, 0}}, {ECampaignAction::Walk, {-650, -414, 0}},
        {ECampaignAction::Floor, {0, 0, 320}, 108},
        {ECampaignAction::Place, {54, -416, 0}}, {ECampaignAction::Walk, {-8, -416, 0}},
        {ECampaignAction::Floor, {0, 0, 320}, 107},
        {ECampaignAction::Place, {-268, 1046, 0}},
        {ECampaignAction::Defeat, FVector::ZeroVector, NativeWadRules::ElymasNotes, TEXT("Elymas")},
        {ECampaignAction::Ceiling, {0, 0, 112}, 68, TEXT("L44 three pressure plates raise gate 68")},
        {ECampaignAction::Place, {-714, 3060, 0}}, {ECampaignAction::Pickup, {-702, 3090, 44}, NativeWadRules::ElmoreKey3},
        {ECampaignAction::Place, {384, 3166, 0}}, {ECampaignAction::Use, {384, 3136, 42}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 42, TEXT("L44 Key 3 opens the Town Hall passage")},
        {ECampaignAction::Place, {408, 3264, 0}}, {ECampaignAction::Walk, {384, 3296, 0}},
    };
    const FCampaignAction Campaign74[] = {
        {ECampaignAction::Place, {208, 1150, 0}}, {ECampaignAction::Use, {208, 1200, 92}},
        {ECampaignAction::Ceiling, {0, 0, 152}, 104, TEXT("L74's switch raises gate 104")},
        {ECampaignAction::Frame, {208, 1200, 92}, INDEX_NONE, TEXT("city-switch")},
        {ECampaignAction::Place, {-2145, 2376, 0}}, {ECampaignAction::Walk, {-2145, 2430, 0}},
    };
    const FCampaignAction Campaign73[] = {
        {ECampaignAction::Place, {-226, 1318, 0}},
        {ECampaignAction::HealthAtLeast, {0, 0, 1800}, INDEX_NONE, TEXT("L73's source recovery sector restores health")},
        {ECampaignAction::Place, {-4382, 1542, 0}}, {ECampaignAction::Walk, {-4363, 1590, 0}},
        {ECampaignAction::Place, {-1029, 2782, 0}},
        {ECampaignAction::Ceiling, {0, 0, 175}, 118},
        {ECampaignAction::Place, {-4562, 1304, 0}}, {ECampaignAction::Walk, {-4610, 1329, 0}},
        {ECampaignAction::Place, {-1029, 2782, 0}},
        {ECampaignAction::Ceiling, {0, 0, 195}, 118},
        {ECampaignAction::Place, {-4321, 1125, 0}}, {ECampaignAction::Walk, {-4345, 1078, 0}},
        {ECampaignAction::Place, {-1029, 2782, 0}},
        {ECampaignAction::Ceiling, {0, 0, 232}, 118, TEXT("L73's three plates raise gate 118 through all three heights")},
        {ECampaignAction::Walk, {-1029, 2864, 0}},
    };
    const FCampaignAction Campaign72[] = {
        {ECampaignAction::Place, {1460, 946, 0}}, {ECampaignAction::Use, {1485, 946, -12}},
        {ECampaignAction::Floor, {0, 0, -56}, 295, TEXT("L72's wall lowers the hidden floor")},
        {ECampaignAction::Place, {-3432, 944, 0}}, {ECampaignAction::Pickup, {-3392, 984, 12}, NativeWadRules::WandOfDisintegration,
            TEXT("the original L72 Wand of Disintegration pickup")},
        {ECampaignAction::Place, {-3382, 1136, 0}}, {ECampaignAction::Walk, {-3310, 1136, 0}},
    };
    const FCampaignAction Campaign32[] = {
        {ECampaignAction::Place, {706, 836, 0}}, {ECampaignAction::Use, {656, 836, 196}},
        {ECampaignAction::Ceiling, {0, 0, 328}, 108}, {ECampaignAction::Ceiling, {0, 0, 280}, 233,
            TEXT("L32's delayed switch opens both gates")},
        {ECampaignAction::Place, {1890, 1535, 0}}, {ECampaignAction::Walk, {1950, 1535, 0}},
    };
    const FCampaignAction Campaign71[] = {
        {ECampaignAction::Place, {-746, -64, 0}}, {ECampaignAction::Walk, {-797, -64, 0}},
        {ECampaignAction::Floor, {0, 0, 200}, 84, TEXT("L71's entry strip raises platform 84 to Z200")},
        {ECampaignAction::Place, {-489, -73, 0}}, {ECampaignAction::Use, {-464, -73, 48}},
        {ECampaignAction::Floor, {0, 0, 16}, 84, TEXT("L71's switch lowers platform 84 to Z16")},
        {ECampaignAction::Floor, {0, 0, 200}, 84, TEXT("L71's platform returns to Z200 after the delay")},
        {ECampaignAction::Place, {500, 4235, 0}}, {ECampaignAction::Walk, {456, 4260, 0}},
    };
    const FCampaignAction Campaign70[] = {
        {ECampaignAction::Place, {400, 617, 0}}, {ECampaignAction::Use, {400, 592, -146}},
        {ECampaignAction::Ceiling, {0, 0, 190}, 56, TEXT("L70's switch raises gate 56")},
        {ECampaignAction::Place, {1894, 496, 0}}, {ECampaignAction::Walk, {1976, 496, 0}},
    };
    const FCampaignAction Campaign61[] = {
        {ECampaignAction::Place, {-832, 2922, 0}}, {ECampaignAction::Walk, {-832, 2982, 0}},
        {ECampaignAction::Ceiling, {0, 0, 380}, 143}, {ECampaignAction::Ceiling, {0, 0, 264}, 62,
            TEXT("L61's pressure plate opens both ceilings")},
        {ECampaignAction::Place, {-1588, -712, 0}}, {ECampaignAction::Pickup, {-1628, -672, 148}, NativeWadRules::ElmoreKey8},
        {ECampaignAction::Place, {-2794, 448, 0}}, {ECampaignAction::Walk, {-2855, 448, 0}},
    };
    const FCampaignAction Campaign40[] = {
        {ECampaignAction::Place, {-236, -1808, 0}}, {ECampaignAction::Use, {-186, -1808, 884}},
        {ECampaignAction::Place, {302, -1808, 0}}, {ECampaignAction::Use, {252, -1808, 884}},
        {ECampaignAction::Ceiling, {0, 0, 444}, 41, TEXT("L40's two switches open gate 41")},
        {ECampaignAction::Place, {2267, 3023, 0}}, {ECampaignAction::Pickup, {2304, 3008, 212}, NativeWadRules::OrbOfDestruction,
            TEXT("the original L40 Orb of Destruction pickup")},
        {ECampaignAction::Place, {2376, 3040, 0}}, {ECampaignAction::Pickup, {2336, 3040, 212}, NativeWadRules::Key4},
        {ECampaignAction::Place, {504, 2664, 0}}, {ECampaignAction::Use, {478, 2632, 440}},
        {ECampaignAction::OpenDoor, {0, 0, 56}, 123, TEXT("L40 Key 4 opens door 123")},
        {ECampaignAction::Frame, {478, 2632, 445}, INDEX_NONE, TEXT("last-key-gate")},
        {ECampaignAction::Place, {-190, 3296, 0}}, {ECampaignAction::Walk, {-122, 3296, 0}},
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
    const FCampaignRoute CampaignRoutes[] = {
        {25, 1, 20, 85, {298, 1636}, Campaign25},
        {20, 1, 60, 138, {-1084, 4630}, Campaign20},
        {60, 1, 0, 284, {-3076, -3}, Campaign60},
        {64, 2, 65, 227, {1984, 2704}, Campaign64},
        {65, 2, 34, 239, {-852, 2282}, Campaign65},
        {34, 2, 151, 180, {912, 1596}, {}},
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
        bool bClassicHudReview = false;
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
        double CampaignStepTime = -1.0;
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

        void BeginCampaignMap(UWorld& Current, ANativeWadPlayer& Player)
        {
            Stop(Player);
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
                : CampaignRoute && CampaignRoute->Map == 60 ? 12.0
                : CampaignRoute && (CampaignRoute->Map == 20 || CampaignRoute->Map == 64 || CampaignRoute->Map == 65) ? 8.0 : 5.0;
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

        bool TickClassicHud(ANativeWadPlayer& Player)
        {
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
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
                if (!Player.GetCarriedItems().IsEmpty() || Player.GetExperience() != 0)
                {
                    return Finish(&Player, TEXT("the classic HUD baseline needs a fresh character"));
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
                    Click({47, 161});
                    ++NextPoint;
                }
                break;
            case 3:
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
            case 4:
                Click({272, 174});
                Click({289, 174});
                if (Player.GetRunes() != TEXT("45"))
                {
                    return Finish(&Player, TEXT("the original rune hit targets did not build the spell"));
                }
                ++NextPoint;
                break;
            case 5:
                if (const int32 Result = Capture(TEXT("runes")); Result != 0)
                {
                    const int32 Mana = Player.GetMana();
                    Click({249, 188});
                    if (Result < 0 || Player.GetMana() >= Mana || !Player.GetRunes().IsEmpty())
                    {
                        return Finish(&Player, TEXT("the original Cast button did not spend mana and clear the runes"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the classic HUD at the L301 spawn, inventory and ready hand, rune buttons and Cast action."));
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
            constexpr int32 HealingPotion = 800;
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
                if (CaptureTownFrame(TEXT("orb-of-the-undead"), bSaved))
                {
                    if (!bSaved || !Player.SaveWadCheckpoint(UndeadOrbSlot))
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

        bool CollectCampaignItem(ANativeWadPlayer& Player, int32 Type, const FVector& Position)
        {
            if (CampaignPickupCount == INDEX_NONE && Player.GetCharacterMovement()->IsMovingOnGround())
            {
                CampaignPickupCount = Player.GetItemCount(Type);
                Aim(Player, Position);
                Press(Player, EKeys::E);
                return false;
            }
            return CampaignPickupCount != INDEX_NONE && Player.GetItemCount(Type) == CampaignPickupCount + 1;
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

        void ReadyCampaignHealingPotion(ANativeWadPlayer& Player)
        {
            if (Player.IsInventoryOpen())
            {
                const int32 Slot = Player.GetCarriedItems().Find(GreaterHealingPotion);
                APlayerController* Controller = Player.GetController<APlayerController>();
                ANativeWadHUD* Hud = Controller ? Controller->GetHUD<ANativeWadHUD>() : nullptr;
                if (Hud && Slot != INDEX_NONE)
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
                const bool bElmore = CampaignRoute->Quest == 2;
                if (bElmore && Player.GetItemCount(GreaterHealingPotion) > 0)
                {
                    if (Player.GetReadyItem() != GreaterHealingPotion || Player.IsInventoryOpen())
                    {
                        ReadyCampaignHealingPotion(Player);
                        return false;
                    }
                    if (Player.GetHealth() < 1050)
                    {
                        Press(Player, EKeys::LeftControl);
                    }
                }
                else if ((CampaignRoute->Map == 20 || CampaignRoute->Map == 60 || bElmore)
                    && (Player.GetHealth() < 1050 || !Player.GetRunes().IsEmpty()))
                {
                    CastCampaignRejuvenate(Player);
                }
                return WalkTo(Player, FVector2D(Step.Target));
            }
            case ECampaignAction::Teleport:
                if (FVector2D(Player.GetActorLocation()).Equals(Step.Destination, 6.0))
                {
                    Stop(Player);
                    return true;
                }
                WalkTo(Player, FVector2D(Step.Target));
                return false;
            case ECampaignAction::Jump:
            {
                const FVector2D Target(Step.Target);
                const bool bOnGround = Player.GetCharacterMovement()->IsMovingOnGround();
                const bool bAtTarget = WalkTo(Player, Target);
                // Keep the running speed and jump when the remaining distance reaches the takeoff range.
                if (!bCampaignActionStarted && bOnGround
                    && FVector2D::DistSquared(Target, FVector2D(Player.GetActorLocation())) <= FMath::Square(Step.Index))
                {
                    Press(Player, EKeys::SpaceBar);
                    bCampaignActionStarted = true;
                    return false;
                }
                return bAtTarget && bOnGround;
            }
            case ECampaignAction::Use:
                if (!Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    return false;
                }
                Aim(Player, Step.Target);
                Press(Player, EKeys::E);
                return true;
            case ECampaignAction::Floor:
            case ECampaignAction::Ceiling:
            case ECampaignAction::OpenDoor:
            {
                FVector2D Heights;
                return Level.GetSectorSurfaceHeights(Step.Index, Heights)
                    && (Step.Action == ECampaignAction::OpenDoor ? Heights.Y - Heights.X >= Step.Target.Z
                        : FMath::IsNearlyEqual(Step.Action == ECampaignAction::Floor ? Heights.X : Heights.Y, Step.Target.Z, 0.01));
            }
            case ECampaignAction::Pickup:
                return CollectCampaignItem(Player, Step.Index, Step.Target);
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
                if (!bCampaignActionStarted && !Drop->IsAvailable() && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    for (TActorIterator<ANativeWadGuardian> It(Player.GetWorld()); It; ++It)
                    {
                        if (It->GetEncounterName() == Step.Label && !It->IsDefeated())
                        {
                            bCampaignActionStarted = It->TakeMeleeHit(Player, It->GetEncounterHealth());
                            break;
                        }
                    }
                }
                return bCampaignActionStarted && Drop->IsAvailable() && CollectCampaignItem(Player, Step.Index, Drop->GetActorLocation());
            }
            case ECampaignAction::Frame:
            {
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
                if (!Player.HasRestoredWadCheckpoint() || Quest.ResultQuest != CampaignLaunch->Quest - 1 || !Quest.bSucceeded)
                {
                    return Finish(&Player, TEXT("the next quest needs the previous successful Town Hall checkpoint"));
                }
                if (Quest.bShowResult)
                {
                    Press(Player, EKeys::Enter);
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
                if (CaptureTownFrame(TEXT("town-success"), bSaved))
                {
                    const FString Slot = FString::Printf(TEXT("NativeWadCampaignTown%dReview"), Reward->Quest);
                    if (!bSaved || !Player.SaveWadCheckpoint(Slot))
                    {
                        return Finish(&Player, TEXT("could not save the successful campaign return"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified campaign quest %d turn-in: %d gold, %d experience."),
                        Reward->Quest, Reward->Gold, Reward->Experience);
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
                if (!Arrival || Quest.Active != QuestId || !Current.GetMapName().EndsWith(FString::Printf(TEXT("L%d"), Destination))
                    || !FVector2D(Player.GetActorLocation()).Equals(Arrival->Spawn, 5.0)
                    || (CampaignLaunch && !MatchesTownCharacter(Player))
                    || (Destination == 32 && !Travel->JournalPages.Contains(603))
                    || (CampaignRoute && (Quest.ResumeMaps.FindRef(QuestId) != Destination || !Travel->VisitedLevels.Contains(Source))))
                {
                    return Finish(&Player, TEXT("the campaign did not reach the authored spawn with its previous state"));
                }
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
            if (!CampaignRoute->Actions.IsValidIndex(NextPoint))
            {
                return Finish(&Player, TEXT("the original exit did not start travel"));
            }
            if (NextPoint == CampaignRoute->Actions.Num() - 1 && !bCampaignExitRecorded)
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
            const FCampaignAction& Step = CampaignRoute->Actions[NextPoint];
            const double Now = Current.GetTimeSeconds();
            if (CampaignStepTime < 0.0)
            {
                CampaignStepTime = Now;
            }
            else if (Now - CampaignStepTime > (Step.Action == ECampaignAction::Rejuvenate
                || Step.Action == ECampaignAction::ManaAtLeast ? 240.0 : 30.0))
            {
                FVector2D Heights;
                if ((Step.Action == ECampaignAction::Floor || Step.Action == ECampaignAction::Ceiling
                    || Step.Action == ECampaignAction::OpenDoor) && (*Level)->GetSectorSurfaceHeights(Step.Index, Heights))
                {
                    UE_LOG(LogTemp, Error, TEXT("Campaign sector %d has floor Z%.6f and ceiling Z%.6f, expected %.6f for action %d."),
                        Step.Index, Heights.X, Heights.Y, Step.Target.Z, int32(Step.Action));
                }
                return Finish(&Player, *FString::Printf(TEXT("L%d step %d timed out waiting for action %d"), CampaignRoute->Map,
                    NextPoint, int32(Step.Action)));
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
                HUD->ClickHUD(HUD->InventorySlotCenter(Button));
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
    const bool bFreshCampaign = Mode == TEXT("freshcampaign");
    const bool bCampaignChain = Mode.StartsWith(TEXT("campaign")) && Mode.EndsWith(TEXT("chain"));
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
        : Mode == TEXT("townscroll") ? ETownReview::Scroll : ETownReview::None;
    const bool bStartsInTown = TownReview == ETownReview::Reload || TownReview == ETownReview::Start
        || TownReview == ETownReview::Scroll || L25Review == EL25Review::ElmoreLaunch
        || UndeadReview == EUndeadReview::Launch || TempleReview == ETempleReview::Launch || CampaignLaunch;
    const bool bHasReviewWorld = bStartsInTown ? World && World->IsGameWorld() && World->GetAuthGameMode<ANativeWadTownGameMode>()
        : GetReviewLevel(World, OutputPath, ImageSize) != nullptr;
    const FWorldContext* Context = bHasReviewWorld ? GEngine->GetWorldContextFromWorld(World) : nullptr;
    const bool bAmbient = Mode == TEXT("ambient");
    const bool bClassicHud = Mode == TEXT("classichud");
    const bool bInventory = Mode == TEXT("inventory") || Mode == TEXT("inventoryreload");
    const bool bItemUse = Mode == TEXT("itemuse") || Mode == TEXT("itemusereload");
    const bool bL22Journey = Mode == TEXT("l31tol22");
    const bool bL22Trap = Mode == TEXT("l22trap");
    const bool bLiftCheckpoint = Mode == TEXT("liftsave") || Mode == TEXT("liftreload");
    const bool bDoorReload = Mode == TEXT("doorreload");
    const bool bStartsInL20 = L25Review == EL25Review::L20Reload || L25Review == EL25Review::L20Scenery || L25Review == EL25Review::L20Exit;
    const FString CampaignMap = CampaignRoute ? FString::Printf(TEXT("L%d"), CampaignRoute->Map) : FString();
    const TCHAR* StartMap = bStartsInTown ? TEXT("WadTown") : CampaignRoute ? *CampaignMap : bStartsInL20 ? TEXT("L20")
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
        : bInventory || bItemUse ? TEXT("L30")
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
