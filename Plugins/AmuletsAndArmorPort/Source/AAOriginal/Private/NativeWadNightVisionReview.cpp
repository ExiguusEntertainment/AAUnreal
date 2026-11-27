#include "NativeWadReview.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NativeWadEncounter.h"
#include "NativeWadImport.h"
#include "NativeWadLevel.h"
#include "NativeWadHudData.h"
#include "NativeWadNightVision.h"
#include "NativeWadShaderReadiness.h"
#include "NativeWadPlayer.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UnrealClient.h"

namespace
{
    struct FNightVisionReview : TSharedFromThis<FNightVisionReview>
    {
        FName WorldHandle;
        FString Output;
        int32 Size = 512, Phase = 0, Frames = 0;
        double Started = FPlatformTime::Seconds();
        bool bFixtureQueued = false, bScreenshot = false;
        TOptional<FKey> Held;
        TWeakObjectPtr<ANativeWadPlayer> OtherPlayer;
        TWeakObjectPtr<ASceneCapture2D> CaptureA, CaptureB;
        TWeakObjectPtr<ACameraActor> ViewOff, ViewOn;
        TArray<TWeakObjectPtr<AActor>> Cards;
        FMinimalViewInfo ActiveView;
        TArray<FColor> HudBefore;
        int32 HudWidth = 0, HudHeight = 0;
        TOptional<float> PreviousTemporalSequence;
        UWorld* World() const
        {
            if (GEngine) { for (const auto& Context : GEngine->GetWorldContexts()) { if (Context.ContextHandle == WorldHandle) { return Context.World(); } } }
            return nullptr;
        }
        void Key(ANativeWadPlayer& Player, FKey Code, EInputEvent Event)
        {
            if (auto* Controller = Player.GetController<APlayerController>())
            { Controller->InputKey(FInputKeyEventArgs(nullptr,IPlatformInputDeviceMapper::Get().GetDefaultInputDevice(),Code,Event,FPlatformTime::Cycles64())); }
        }
        void Press(ANativeWadPlayer& Player,FKey Code) { Key(Player,Code,IE_Pressed); Held=Code; }
        bool Finish(ANativeWadPlayer* Player, const TCHAR* Failure = nullptr)
        {
            if (PreviousTemporalSequence)
            {
                if (auto* Sequence=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Test.FreezeTemporalSequences")))
                { Sequence->Set(PreviousTemporalSequence.GetValue(),ECVF_SetByCode); }
                PreviousTemporalSequence.Reset();
            }
            if (Player)
            {
                if (Held) { Key(*Player,Held.GetValue(),IE_Released); }
                if (auto* Controller=Player->GetController<APlayerController>()) { Controller->SetViewTarget(Player); }
            }
            for (const auto& Card:Cards) { if (Card.IsValid()) { Card->Destroy(); } }
            for (AActor* Actor : {static_cast<AActor*>(CaptureA.Get()),static_cast<AActor*>(CaptureB.Get()),
                static_cast<AActor*>(ViewOff.Get()),static_cast<AActor*>(ViewOn.Get()),static_cast<AActor*>(OtherPlayer.Get())})
            { if (Actor) { Actor->Destroy(); } }
            if (Failure) { UE_LOG(LogTemp,Error,TEXT("NightVision rendered review failed: %s"),Failure); }
            UE_LOG(LogTemp,Display,TEXT("Native WAD port review finished"));
            return false;
        }
        FString Filename(const TCHAR* Name) const
        { return FPaths::GetPath(Output)/(FPaths::GetBaseFilename(Output)+TEXT("-")+Name+TEXT(".png")); }
        ASceneCapture2D* Camera(UWorld& Current, const FMinimalViewInfo& View)
        {
            auto* Actor=Current.SpawnActor<ASceneCapture2D>(View.Location,View.Rotation);
            if (!Actor) { return nullptr; }
            auto* Capture=Actor->GetCaptureComponent2D();
            auto* Target=NewObject<UTextureRenderTarget2D>(Actor);
            Target->RenderTargetFormat=RTF_RGBA8;
            Target->InitAutoFormat(Size,Size);
            Target->UpdateResourceImmediate(true);
            Capture->TextureTarget=Target;
            Capture->FOVAngle=View.FOV;
            Capture->CaptureSource=ESceneCaptureSource::SCS_FinalColorLDR;
            Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;
            Capture->bAlwaysPersistRenderingState=true;
            Capture->ShowFlags.SetTemporalAA(false);Capture->ShowFlags.SetMotionBlur(false);
            Capture->PostProcessBlendWeight=1;
            Capture->PostProcessSettings=View.PostProcessSettings;
            ConfigureWadLumen(Capture->PostProcessSettings);
            return Actor;
        }
        bool Read(ASceneCapture2D& Actor, TArray<FColor>& Pixels, const TCHAR* Name)
        {
            auto* Capture=Actor.GetCaptureComponent2D();Capture->CaptureScene();FlushRenderingCommands();
            auto* Resource=Capture->TextureTarget->GameThread_GetRenderTargetResource();
            if (!Resource||!Resource->ReadPixels(Pixels)||Pixels.Num()!=Size*Size) { return false; }
            TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(Size,Size,Pixels,Png);
            return !Png.IsEmpty()&&FFileHelper::SaveArrayToFile(Png,*Filename(Name));
        }
        int32 Screenshot(const TCHAR* Name)
        {
            if (!bScreenshot)
            {
                bScreenshot=true;Frames=0;
                FScreenshotRequest::RequestScreenshot(Filename(Name),true,false);
                return 0;
            }
            if (++Frames<20) { return 0; }
            bScreenshot=false;Frames=0;
            return IFileManager::Get().FileExists(*Filename(Name))?1:-1;
        }
        bool LoadScreenshot(const TCHAR* Name,TArray<FColor>& Pixels,int32& Width,int32& Height)
        {
            TArray<uint8> Bytes;FImage Image;
            if (!FFileHelper::LoadFileToArray(Bytes,*Filename(Name))||!FImageUtils::DecompressImage(Bytes.GetData(),Bytes.Num(),Image)
                ||Image.Format!=ERawImageFormat::BGRA8||Image.NumSlices!=1) { return false; }
            Width=Image.SizeX;Height=Image.SizeY;
            const auto Colors=Image.AsBGRA8();Pixels.Append(Colors.GetData(),int32(Colors.Num()));return true;
        }
        bool Tick()
        {
            UWorld* Current=World();auto* Controller=Current?Current->GetFirstPlayerController():nullptr;
            auto* Player=Controller?Cast<ANativeWadPlayer>(Controller->GetPawn()):nullptr;
            if (FPlatformTime::Seconds()-Started>120) { return Finish(Player,TEXT("timed out")); }
            if (!Player) { return true; }
            const auto* Hud=Controller->GetHUD<ANativeWadHUD>();
            if (!Hud||!Hud->HasClassicHud()) { return true; }
            if (Held) { Key(*Player,Held.GetValue(),IE_Released);Held.Reset(); }
            for (TActorIterator<ANativeWadGuardian> Enemy(Current);Enemy;++Enemy)
            { Enemy->SetActorTickEnabled(false);Enemy->GetCharacterMovement()->StopMovementImmediately();Enemy->GetCharacterMovement()->DisableMovement(); }
            if (Phase==0)
            {
                if (!Current->URL.HasOption(TEXT("WadNightFixture")))
                {
                    if (!bFixtureQueued)
                    {
                        bFixtureQueued=true;
                        UGameplayStatics::OpenLevel(Player,FName(TEXT("/Game/Maps/L30")),true,
                            TEXT("WadNightFixture=1?WadClass=2?WadCreated=1?WadRuneRules=1?WadLevel=100")
                            TEXT("?WadAttributes=100,100,100,100,100,100?WadMaxHealth=1700?WadHealth=1700?WadMaxMana=30000?WadMana=30000")
                            TEXT("?WadFood=0?WadWater=0?WadReady=-1?WadItems=300,301,302,303,304,305,306,307,308?WadOwnedEffects=1"));
                    }
                    return true;
                }
                const auto* Stencil=IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth"));
                if (!Stencil||Stencil->GetInt()!=3) { return Finish(Player,TEXT("r.CustomDepth=3 is required for a meaningful sky proof")); }
                bool bMarkedSky=false;
                for (TActorIterator<ANativeWadLevel> Level(Current);Level;++Level)
                {
                    TInlineComponentArray<UStaticMeshComponent*> Components;Level->GetComponents(Components);
                    for (const auto* Component:Components)
                    { bMarkedSky|=Component->GetName().Contains(TEXT("AuthoredBackdrop"))&&Component->bRenderCustomDepth&&Component->CustomDepthStencilValue==NativeWadNightVision::SkyStencil; }
                }
                if (!bMarkedSky) { return Finish(Player,TEXT("the original saved sky did not receive its constant runtime classification")); }
                UE_LOG(LogTemp,Display,TEXT("Starting CONTROLLED NightVision swatch/two-camera fixture, not campaign progress or exact palette-shade parity."));
                Phase=1;return true;
            }
            if (Phase==1)
            {
                if (Player->GetRunes().IsEmpty()) { Press(*Player,EKeys::NumPadNine); }
                else if (Player->GetRunes()==TEXT("9")) { Press(*Player,EKeys::NumPadFive); }
                else if (Player->GetRunes()==TEXT("95")) { Press(*Player,EKeys::NumPadSeven); }
                else if (Player->GetRunes()==TEXT("957")) { Press(*Player,EKeys::RightMouseButton);Phase=2; }
                else { return Finish(Player,TEXT("ordinary source36 rune sequence diverged")); }
                return true;
            }
            if (Phase==2)
            {
                if (Player->GetSuccessfulCasts()!=1||Player->GetMana()!=27000||Player->GetNightVisionPower()!=80
                    ||Player->GetOwnedEffectRemaining(TEXT("spell:36"))<=0) { return Finish(Player,TEXT("normal source36 cast/cost/owner failed")); }
                Player->GetCharacterMovement()->StopMovementImmediately();Player->GetCharacterMovement()->DisableMovement();
                Player->SetActorLocation(FVector(0,0,5000),false,nullptr,ETeleportType::TeleportPhysics);
                Controller->SetControlRotation(FRotator::ZeroRotator);
                Player->CalcCamera(0,ActiveView);
                if (ActiveView.PostProcessSettings.WeightedBlendables.Array.IsEmpty())
                {
                    const auto* Catalog=UNativeWadHudData::LoadCatalog();
                    auto* Parent=Catalog?Catalog->NightVisionPostProcess.Get():nullptr;
                    // Ordinary owner/cost assertions above still hold. Wait only
                    // for the actual runtime's pending renderer proof, within120s.
                    if (Parent&&!NativeWadNightVision::IsRenderingReady(*Parent)) { return true; }
                    return Finish(Player,TEXT("the actual player's ready camera has no private postprocess MID"));
                }
                FActorSpawnParameters Params;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                OtherPlayer=Current->SpawnActor<ANativeWadPlayer>(Player->GetActorLocation(),FRotator::ZeroRotator,Params);
                if (!OtherPlayer.IsValid()) { return Finish(Player,TEXT("could not create independent viewerB")); }
                OtherPlayer->SetActorEnableCollision(false);OtherPlayer->SetActorTickEnabled(false);OtherPlayer->GetCharacterMovement()->DisableMovement();
                FMinimalViewInfo OtherView;OtherPlayer->CalcCamera(0,OtherView);
                OtherView.Location=ActiveView.Location;OtherView.Rotation=ActiveView.Rotation;OtherView.FOV=ActiveView.FOV;
                if (OtherPlayer->GetNightVisionPower()!=0||!OtherView.PostProcessSettings.WeightedBlendables.Array.IsEmpty())
                { return Finish(Player,TEXT("independent viewerB incorrectly inherited viewerA's vision")); }
                FMinimalViewInfo Baseline=ActiveView;Baseline.PostProcessSettings.WeightedBlendables.Array.Reset();
                // Compare identical sampling sequences: independent captures otherwise
                // differ at sky edges and by 1-2 quantization codes without any effect.
                if (auto* Sequence=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Test.FreezeTemporalSequences")))
                { PreviousTemporalSequence=Sequence->GetFloat();Sequence->Set(1.0f,ECVF_SetByCode); }
                CaptureA=Camera(*Current,Baseline);CaptureB=Camera(*Current,OtherView);
                if (!CaptureA.IsValid()||!CaptureB.IsValid()) { return Finish(Player,TEXT("could not create both real capture views")); }
                auto* Plane=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane"));
                auto* Parent=LoadObject<UMaterialInterface>(nullptr,NativeWadNightVision::SwatchMaterialPath);
                if (!Plane||!Parent) { return Finish(Player,TEXT("saved controlled swatch material is missing")); }
                // A loaded MID setter can succeed even while the renderer uses a fallback.
                // Explicitly submit/finish missing permutations at this actual feature/quality.
                auto* VisionParent=::Cast<UMaterialInterface>(ActiveView.PostProcessSettings.WeightedBlendables.Array[0].Object);
                if (!VisionParent) { return Finish(Player,TEXT("actual camera blendable is not a material")); }
                const bool SwatchReady=NativeWadShaderReadiness::Prepare(*Parent);
                const bool VisionReady=NativeWadShaderReadiness::Prepare(*VisionParent);
                if (!SwatchReady||!VisionReady) { return Finish(Player,TEXT("saved vision/swatch shader permutations are incomplete or failed; fallback pixels cannot prove this feature")); }
                FLinearColor DefaultSwatch;float DefaultPower=-1;
                if (!Parent->GetVectorParameterValue(FMaterialParameterInfo(TEXT("SwatchColor")),DefaultSwatch)
                    ||!DefaultSwatch.Equals(FLinearColor::White,UE_SMALL_NUMBER))
                { return Finish(Player,TEXT("saved swatch graph is missing its white SwatchColor default")); }
                if (Parent->GetMaterial()->MaterialDomain!=MD_Surface||!Parent->GetShadingModels().HasShadingModel(MSM_Unlit))
                { return Finish(Player,TEXT("saved swatch is not an unlit surface material")); }
                if (!VisionParent->GetMaterial()->GetScalarParameterValue(FMaterialParameterInfo(NativeWadNightVision::PowerParameter),DefaultPower)||DefaultPower!=0)
                { return Finish(Player,TEXT("saved vision parent is missing its neutral default power0")); }
                UE_LOG(LogTemp,Display,TEXT("NightVision saved graph diagnostics: swatch default%s, vision default%.3f, feature%d; both actual shader maps complete"),*DefaultSwatch.ToString(),DefaultPower,int32(GMaxRHIFeatureLevel));

                // Neutral classified sky card makes a measurable exclusion proof even
                // when the original magenta sky happens to be clipped at display white.
                for (int32 Index=0;Index<4;++Index)
                {
                    auto* Card=Current->SpawnActor<AActor>();if (!Card) { return Finish(Player,TEXT("swatch creation failed")); }
                    auto* Mesh=NewObject<UStaticMeshComponent>(Card);Card->SetRootComponent(Mesh);Mesh->SetStaticMesh(Plane);
                    auto* Material=UMaterialInstanceDynamic::Create(Parent,Card);
                    // Stay above the tonemapper's near-black toe: .003 produced
                    // less than one 8-bit code value, too little for this proof.
                    const float Color=Index==3?0.05f:Index==1?0.8f:0.03f;
                    Material->SetVectorParameterValue(TEXT("SwatchColor"),FLinearColor(Color,Color,Color,1));Mesh->SetMaterial(0,Material);
                    FLinearColor Actual;
                    if (!Material->GetVectorParameterValue(FMaterialParameterInfo(TEXT("SwatchColor")),Actual)
                        ||!Actual.Equals(FLinearColor(Color,Color,Color,1),UE_SMALL_NUMBER))
                    { return Finish(Player,TEXT("controlled swatch MID failed exact parameter readback")); }

                    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);Mesh->SetCastShadow(false);Mesh->SetAffectDistanceFieldLighting(false);
                    Card->SetActorLocation(ActiveView.Location+FVector(Index==3?400:180,Index==3?0:(Index-1)*75,0));
                    Card->SetActorRotation(FRotator(90,0,0));Card->SetActorScale3D(FVector(Index==3?5.0:0.5));
                    if (Index==3) { NativeWadNightVision::MarkSky(*Mesh); }
                    Mesh->RegisterComponent();Cards.Add(Card);
                    UE_LOG(LogTemp,Display,TEXT("NightVision card%d color%s at%s bounds%s registered%d"),Index,*Actual.ToString(),*Mesh->GetComponentLocation().ToString(),*Mesh->Bounds.BoxExtent.ToString(),Mesh->IsRegistered());
                }
                Phase=3;Frames=0;return true;
            }
            if (Phase==3)
            {
                CaptureA->GetCaptureComponent2D()->CaptureScene();CaptureB->GetCaptureComponent2D()->CaptureScene();
                if (++Frames<12) { return true; }
                Phase=4;Frames=0;return true;
            }
            if (Phase==4)
            {
                TArray<FColor> A0,A1,B0,B1;
                if (!Read(*CaptureA,A0,TEXT("swatch-a-off"))||!Read(*CaptureB,B0,TEXT("swatch-b-before")))
                { return Finish(Player,TEXT("baseline GPU capture/readback failed")); }
                auto* A=CaptureA->GetCaptureComponent2D();A->PostProcessSettings=ActiveView.PostProcessSettings;ConfigureWadLumen(A->PostProcessSettings);
                if (!Read(*CaptureA,A1,TEXT("swatch-a-on"))||!Read(*CaptureB,B1,TEXT("swatch-b-after")))
                { return Finish(Player,TEXT("active GPU capture/readback failed")); }
                const auto Mean=[&](const TArray<FColor>& Pixels,int32 X,int32 Y)
                {
                    double Total=0;for (int32 Dy=-4;Dy<=4;++Dy) { for (int32 Dx=-4;Dx<=4;++Dx)
                    { const FColor C=Pixels[(Y+Dy)*Size+X+Dx];Total+=(C.R+C.G+C.B)/3.0; } }return Total/81.0;
                };
                const int32 DarkX=FMath::RoundToInt(Size*0.5-75.0*Size/(360.0*FMath::Tan(FMath::DegreesToRadians(ActiveView.FOV*0.5))));
                const double Dark0=Mean(A0,DarkX,Size/2),Dark1=Mean(A1,DarkX,Size/2);
                UE_LOG(LogTemp,Display,TEXT("NightVision GPU samples: darkX%d dark%.3f->%.3f bright%.3f->%.3f neutralSky%.3f->%.3f originalSky%.3f->%.3f"),
                    DarkX,Dark0,Dark1,Mean(A0,Size/2,Size/2),Mean(A1,Size/2,Size/2),Mean(A0,Size/2,Size/4),Mean(A1,Size/2,Size/4),Mean(A0,Size/16,Size/16),Mean(A1,Size/16,Size/16));
                if (Dark0<2||Dark0>200||Dark1<Dark0*1.2+2) { return Finish(Player,TEXT("actual shader failed measurable dark foreground improvement over a sky stencil background")); }
                if (FMath::Abs(Mean(A0,Size/2,Size/2)-Mean(A1,Size/2,Size/2))>2)
                { return Finish(Player,TEXT("bright reference swatch changed materially")); }
                // Classified neutral sky and actual original sky are compared independently.
                for (const FIntPoint Point:{FIntPoint(Size/2,Size/4),FIntPoint(Size/16,Size/16)})
                { if (FMath::Abs(Mean(A0,Point.X,Point.Y)-Mean(A1,Point.X,Point.Y))>1) { return Finish(Player,TEXT("visible classified/original sky was modified by private NightVision")); } }
                int32 OtherMax=0;
                for (int32 Index=0;Index<B0.Num();++Index)
                { OtherMax=FMath::Max3(OtherMax,FMath::Abs(int32(B0[Index].R)-B1[Index].R),FMath::Abs(int32(B0[Index].G)-B1[Index].G));OtherMax=FMath::Max(OtherMax,FMath::Abs(int32(B0[Index].B)-B1[Index].B)); }
                if (OtherMax>1) { return Finish(Player,TEXT("changing A's private postprocess changed independent viewB")); }
                UE_LOG(LogTemp,Display,TEXT("NightVision GPU proof: dark grey %.2f -> %.2f, independentB max channel difference%d; sky/background depth exclusion passed."),Dark0,Dark1,OtherMax);
                ViewOff=Current->SpawnActor<ACameraActor>(ActiveView.Location,ActiveView.Rotation);
                ViewOn=Current->SpawnActor<ACameraActor>(ActiveView.Location,ActiveView.Rotation);
                if (!ViewOff.IsValid()||!ViewOn.IsValid()) { return Finish(Player,TEXT("HUD comparison cameras unavailable")); }
                for (auto* Camera:{ViewOff->GetCameraComponent(),ViewOn->GetCameraComponent()})
                {
                    Camera->SetFieldOfView(ActiveView.FOV);
                    // ACameraActor defaults to constrained 16:9; retain the actual
                    // player's framing so the original HUD uses identical coordinates.
                    Camera->SetAspectRatio(ActiveView.AspectRatio);
                    Camera->SetConstraintAspectRatio(ActiveView.bConstrainAspectRatio);
                    Camera->SetPostProcessBlendWeight(1);Camera->PostProcessSettings=ActiveView.PostProcessSettings;ConfigureWadLumen(Camera->PostProcessSettings);
                }
                ViewOff->GetCameraComponent()->PostProcessSettings.WeightedBlendables.Array.Reset();
                Controller->SetViewTarget(ViewOff.Get());Phase=5;Frames=0;return true;
            }
            if (Phase==5)
            {
                if (++Frames<3) { return true; }Frames=0;Phase=6;return true;
            }
            if (Phase==6)
            {
                const int32 Result=Screenshot(TEXT("hud-off"));if (!Result) { return true; }
                if (Result<0||!LoadScreenshot(TEXT("hud-off"),HudBefore,HudWidth,HudHeight)) { return Finish(Player,TEXT("could not read original HUD baseline screenshot")); }
                Controller->SetViewTarget(ViewOn.Get());Phase=7;return true;
            }
            if (Phase==7)
            {
                if (++Frames<3) { return true; }Frames=0;Phase=8;return true;
            }
            if (Phase==8)
            {
                const int32 Result=Screenshot(TEXT("hud-on"));if (!Result) { return true; }
                TArray<FColor> After;int32 Width=0,Height=0;
                if (Result<0||!LoadScreenshot(TEXT("hud-on"),After,Width,Height)||Width!=HudWidth||Height!=HudHeight)
                { return Finish(Player,TEXT("could not read comparable active HUD screenshot")); }
                const double Scale=FMath::Min(Width/320.0,Height/200.0);
                const double OX=(Width-320*Scale)/2,OY=(Height-200*Scale)/2;
                int32 Compared=0,Changed=0;
                for (int32 Y=FMath::CeilToInt(OY+151*Scale);Y<FMath::FloorToInt(OY+199*Scale);++Y)
                { for (int32 X=FMath::CeilToInt(OX+4*Scale);X<FMath::FloorToInt(OX+316*Scale);++X)
                    {
                        const double HX=(X-OX)/Scale,HY=(Y-OY)/Scale;
                        // Original potion-frame and sine-wave gauges animate with source time.
                        // Compare every other original HUD pixel; those two animation regions are reported explicitly.
                        if ((HX>=60&&HX<=96&&HY>=154&&HY<=193)||(HX>=176&&HX<=260&&HY>=154&&HY<=180)) { continue; }
                        ++Compared;const FColor A=HudBefore[Y*Width+X],B=After[Y*Width+X];Changed+=(A.R!=B.R||A.G!=B.G||A.B!=B.B);
                    } }
                if (Compared<1000||Changed) { return Finish(Player,TEXT("private world postprocess changed original static HUD pixels")); }
                UE_LOG(LogTemp,Display,TEXT("Verified controlled NightVision GPU/two-view proof and%d unchanged original static HUD pixels (source-animated vial/wave regions excluded). Native SDR shadow gain, not exact palette-shade rendering."),Compared);
                return Finish(Player);
            }
            return true;
        }
    };
}

bool StartNativeWadNightVisionReview(UWorld* World,const FString& OutputPath,int32 ImageSize)
{
    if (!World||!World->GetMapName().EndsWith(TEXT("L30"))||FParse::Param(FCommandLine::Get(),TEXT("nullrhi"))) { return false; }
    const auto* Context=GEngine?GEngine->GetWorldContextFromWorld(World):nullptr;
    if (!Context) { return false; }
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath),true);
    const auto Review=MakeShared<FNightVisionReview>();Review->WorldHandle=Context->ContextHandle;
    Review->Output=OutputPath;Review->Size=FMath::Clamp(ImageSize,256,1024);
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float){return Review->Tick();}));
    return true;
}
