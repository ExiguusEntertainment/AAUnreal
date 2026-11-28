#include "NativeWadSourceLightingReview.h"
#include "NativeWadReview.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadEncounter.h"
#include "NativeWadCheckpoint.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/Package.h"

namespace
{
    constexpr const TCHAR* SourceLightingReviewSlot=TEXT("NativeWadSourceLightingReview");
    struct FSourceLightingReview
    {
        FName WorldHandle;FString Output;int32 Size=512,Phase=0,Frames=0;bool bReload=false;
        double Started=FPlatformTime::Seconds(),RunStarted=0.0,LastSampleTime=0.0;uint64 StartSteps=0,FrozenLookups=0;
        TWeakObjectPtr<ANativeWadLevel> Level;TWeakObjectPtr<ASceneCapture2D> Camera;
        FWadSourceLightState Frozen;TArray<int32> BDisplay;TArray<FLinearColor> BPixels;TArray<double> Samples;
        TOptional<float> Temporal;int32 ReviewSector=INDEX_NONE;double MaxChanged=0.0,BaselineNoise=0.0;int32 ChangedPixels=0;
        TMap<TWeakObjectPtr<AActor>,bool> VisualTicks;
        UWorld* World() const
        {if(GEngine){for(const auto& C:GEngine->GetWorldContexts()){if(C.ContextHandle==WorldHandle){return C.World();}}}return nullptr;}
        bool Finish(const TCHAR* Failure=nullptr)
        {
            if(Temporal){if(auto* C=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Test.FreezeTemporalSequences"))){C->Set(Temporal.GetValue(),ECVF_SetByCode);}}
            if(auto* L=Level.Get())
            {
                FString Error;
                if(!L->IsSourceLightFeedbackEnabled()){L->SetSourceLightFeedbackEnabled(true,{},Error);L->RestoreSourceLightState(Frozen,Error);}
                L->SetActorTickEnabled(true);
            }
            for(const auto& Entry:VisualTicks){if(Entry.Key.IsValid()){Entry.Key->SetActorTickEnabled(Entry.Value);}}
            if(Camera.IsValid()){Camera->Destroy();}
            if(Failure){UE_LOG(LogTemp,Error,TEXT("Source lighting candidate review failed: %s"),Failure);}
            else{UE_LOG(LogTemp,Display,TEXT("Source lighting candidate rendering/state checks passed; game-thread timings recorded for review. Native RGB ambient approximation, sprites unchanged."));}
            UE_LOG(LogTemp,Display,TEXT("Native WAD port review finished"));return false;
        }
        bool Pixels(TArray<FLinearColor>& Result,const TCHAR* Label)
        {
            auto* Actor=Camera.Get();if(!Actor){return false;}auto* C=Actor->GetCaptureComponent2D();C->CaptureScene();FlushRenderingCommands();
            auto* R=C->TextureTarget->GameThread_GetRenderTargetResource();
            return R&&R->ReadLinearColorPixels(Result)&&Result.Num()==Size*Size&&!Result.ContainsByPredicate([](const FLinearColor& P){return !FMath::IsFinite(P.R)||!FMath::IsFinite(P.G)||!FMath::IsFinite(P.B);})&&SaveWadReviewFrame(*C,Output,Label,Size);
        }
        bool Tick()
        {
            UWorld* W=World();auto* PC=W?W->GetFirstPlayerController():nullptr;auto* Player=PC?Cast<ANativeWadPlayer>(PC->GetPawn()):nullptr;
            if(FPlatformTime::Seconds()-Started>120){return Finish(TEXT("timed out waiting for actual candidate/render/save state"));}
            if(!W||!Player){return true;}
            auto* L=Level.Get();
            if(Phase==0)
            {
                for(TActorIterator<ANativeWadLevel> It(W);It;++It){if(L){return Finish(TEXT("multiple native levels"));}L=*It;}
                if(!L||!L->IsSourceLightFeedbackEnabled()||L->GetSourceLightState().Version!=1){return Finish(TEXT("candidate model did not initialize"));}
                Level=L;
                // Controlled candidate-only review: no campaign progress or player grants.
                for(TActorIterator<ANativeWadGuardian> It(W);It;++It)
                {It->SetActorTickEnabled(false);It->GetCharacterMovement()->StopMovementImmediately();It->GetCharacterMovement()->DisableMovement();}
                Player->GetCharacterMovement()->StopMovementImmediately();
                if(bReload)
                {
                    const auto* Save=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(SourceLightingReviewSlot,0));
                    if(!Save||!W->URL.HasOption(TEXT("WadLoad"))){return Finish(TEXT("restart mode needs actual fixture checkpoint loading"));}
                    const auto Current=L->GetSourceLightState();const auto& Saved=Save->Level.SourceLighting;
                    if(Current.Version!=Saved.Version||Current.SourceMapSha1!=Saved.SourceMapSha1||Current.CompletedSteps<Saved.CompletedSteps
                        ||L->GetRestoredSourceLightingDigest()!=NativeWadLightRules::StateDigest(Saved))
                    {return Finish(TEXT("restart lost source identity/version/clock progress"));}
                    for(const auto& A:L->GetSourceLightingMetadata().Animations)
                    {
                        const uint16 Expected=uint16(uint64(Saved.Phases[A.Sector])+((Current.CompletedSteps-Saved.CompletedSteps)*A.Rate<<3));
                        if(Current.Phases[A.Sector]!=Expected){return Finish(TEXT("restart did not continue the saved source phase"));}
                    }
                    UE_LOG(LogTemp,Display,TEXT("Source lighting actual restart loaded source bytes/phases/debt; savedSteps%llu currentSteps%llu"),Saved.CompletedSteps,Current.CompletedSteps);
                }
                StartSteps=L->GetSourceLightState().CompletedSteps;RunStarted=LastSampleTime=W->GetTimeSeconds();Phase=1;return true;
            }
            if(!L||Player->IsDefeated()){return Finish(TEXT("candidate/player became unavailable"));}
            if(Phase==1)
            {
                const auto State=L->GetSourceLightState();
                if(L->GetSourceLightingLastStepCount()>NativeWadLightRules::MaximumStepsPerFrame){return Finish(TEXT("fixed-step work budget exceeded"));}
                if(W->GetTimeSeconds()!=LastSampleTime){Samples.Add(L->GetSourceLightingLastMilliseconds());LastSampleTime=W->GetTimeSeconds();}
                if(W->GetTimeSeconds()-RunStarted<2.0||State.CompletedSteps-StartSteps<70||State.PendingTicks>=1.0){return true;}
                if(!Player->GetCharacterMovement()->IsMovingOnGround()){return true;}
                if(TActorIterator<ANativeWadBolt>(W)){return true;}
                Frozen=State;FrozenLookups=L->GetSourceLightingLookupResolutions();L->SetActorTickEnabled(false);
                for(TActorIterator<AActor> It(W);It;++It)
                {
                    if(*It==L||*It==Player){continue;}
                    VisualTicks.Add(TWeakObjectPtr<AActor>(*It),It->IsActorTickEnabled());It->SetActorTickEnabled(false);
                }
                for(int32 I=0;I<State.Levels.Num();++I){BDisplay.Add(L->GetSectorLightLevel(I));}
                auto ReviewOwners=L->GetSourceLightingMetadata().Illuminators;
                for(auto& O:ReviewOwners)
                {
                    O.CenterSector=L->GetSourceIlluminationOwnerSector(O.ThingId);
                    if(IsValid(O.BoundActor))
                    {const FVector Local=L->GetActorTransform().InverseTransformPosition(O.BoundActor->GetActorLocation());O.CenterPosition=FVector2D(Local.X,Local.Y);}
                }
                FString Error;
                if(!L->SetSourceLightFeedbackEnabled(false,{},Error)){return Finish(TEXT("could not publish legacy comparison"));}
                int32 Difference=0;FVector2D XY;
                for(const auto& O:ReviewOwners)
                {
                    if(O.CenterSector==INDEX_NONE){continue;}
                    const int32 D=FMath::Abs(BDisplay[O.CenterSector]-L->GetSectorLightLevel(O.CenterSector));
                    if(D>Difference){Difference=D;ReviewSector=O.CenterSector;XY=O.CenterPosition;}
                }
                if(Difference<8||ReviewSector==INDEX_NONE){return Finish(TEXT("candidate has no measurable owner-sector lighting difference to review"));}
                if(!L->SetSourceLightFeedbackEnabled(true,{},Error)||!L->RestoreSourceLightState(Frozen,Error)){return Finish(TEXT("could not restore exact B comparison state"));}
                L->SetActorTickEnabled(false);FVector2D Range;
                if(!L->GetSectorSurfaceHeights(ReviewSector,Range)||Range.Y-Range.X<16){return Finish(TEXT("comparison sector has no camera clearance"));}
                const FVector Location=L->GetActorTransform().TransformPosition(FVector(XY,Range.X+FMath::Min(48.0,(Range.Y-Range.X)*0.5)));
                Camera=SpawnWadReviewCamera(*W,Size,Location,FRotator(-70,0,0));if(!Camera.IsValid()){return Finish(TEXT("could not create real scene capture"));}
                auto* C=Camera->GetCaptureComponent2D();C->ShowFlags.SetTemporalAA(false);C->ShowFlags.SetAntiAliasing(false);C->ShowFlags.SetMotionBlur(false);C->ShowFlags.SetGrain(false);
                if(auto* Sequence=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Test.FreezeTemporalSequences")))
                {Temporal=Sequence->GetFloat();Sequence->Set(1.0f,ECVF_SetByCode);}
                Phase=2;Frames=0;return true;
            }
            if(Camera.IsValid()){Camera->GetCaptureComponent2D()->CaptureScene();}
            if(++Frames<20){return true;}Frames=0;
            if(Phase==2)
            {
                if(!Pixels(BPixels,bReload?TEXT("reloaded-B"):TEXT("source-B"))){return Finish(TEXT("actual B capture read/save failed"));}
                Phase=4;return true;
            }
            if(Phase==4)
            {
                TArray<FLinearColor> Control;
                if(!Pixels(Control,bReload?TEXT("reloaded-B-control"):TEXT("source-B-control"))){return Finish(TEXT("B control capture failed"));}
                for(int32 I=0;I<Control.Num();++I)
                {BaselineNoise=FMath::Max(BaselineNoise,double(FMath::Max3(FMath::Abs(BPixels[I].R-Control[I].R),FMath::Abs(BPixels[I].G-Control[I].G),FMath::Abs(BPixels[I].B-Control[I].B))));}
                FString Error;if(!L->SetSourceLightFeedbackEnabled(false,{},Error)){return Finish(TEXT("legacy comparison toggle failed"));}
                L->SetActorTickEnabled(false);Phase=3;return true;
            }
            if(Phase==3)
            {
                TArray<FLinearColor> Legacy;
                if(!Pixels(Legacy,bReload?TEXT("reloaded-legacy"):TEXT("legacy"))){return Finish(TEXT("actual legacy capture read/save failed"));}
                for(int32 I=0;I<Legacy.Num();++I)
                {
                    const double D=FMath::Max3(FMath::Abs(BPixels[I].R-Legacy[I].R),FMath::Abs(BPixels[I].G-Legacy[I].G),FMath::Abs(BPixels[I].B-Legacy[I].B));
                    ChangedPixels+=D>FMath::Max(1.e-4,3.0*BaselineNoise);MaxChanged=FMath::Max(MaxChanged,D);
                }
                if(ChangedPixels<16){return Finish(TEXT("real rendered source/legacy surfaces did not measurably differ"));}
                FString Error;if(!L->SetSourceLightFeedbackEnabled(true,{},Error)||!L->RestoreSourceLightState(Frozen,Error)){return Finish(TEXT("could not restore B after legacy capture"));}
                L->SetActorTickEnabled(false);
                if(!bReload&&!Player->SaveWadCheckpoint(SourceLightingReviewSlot)){return Finish(TEXT("normal actual player checkpoint save refused"));}
                Samples.Sort();double Total=0;for(double Sample:Samples){Total+=Sample;}
                const double Mean=Samples.IsEmpty()?0.0:Total/Samples.Num(),P95=Samples.IsEmpty()?0.0:Samples[FMath::Clamp(FMath::CeilToInt(Samples.Num()*0.95)-1,0,Samples.Num()-1)];
                const FString Text=FString::Printf(TEXT("{\n  \"mode\": \"%s\",\n  \"source_sha1\": \"%s\",\n  \"state_digest\": \"%s\",\n  \"steps\": %llu,\n  \"pending_ticks\": %.17g,\n  \"cooked_lookup_resolutions\": %llu,\n  \"perf_samples\": %d,\n  \"mean_game_thread_ms\": %.8f,\n  \"p95_game_thread_ms\": %.8f,\n  \"owner_sector\": %d,\n  \"baseline_control_max_difference\": %.8f,\n  \"changed_pixels_above_3x_control\": %d,\n  \"max_linear_rgb_difference\": %.8f\n}\n"),
                    bReload?TEXT("new-process restart"):TEXT("controlled candidate"),*Frozen.SourceMapSha1,*NativeWadLightRules::StateDigest(Frozen),Frozen.CompletedSteps,Frozen.PendingTicks,FrozenLookups,Samples.Num(),Mean,P95,ReviewSector,BaselineNoise,ChangedPixels,MaxChanged);
                if(!FFileHelper::SaveStringToFile(Text,*(FPaths::GetPath(Output)/(FPaths::GetBaseFilename(Output)+TEXT("-metrics.json"))))){return Finish(TEXT("metrics could not be persisted"));}
                UE_LOG(LogTemp,Display,TEXT("Source lighting rendered candidate sector%d changedPixels%d above3xBcontrolNoise%.8f; normal-tick samples%d mean%.5fms p95%.5fms fixedSteps%llu debt%.6f"),ReviewSector,ChangedPixels,BaselineNoise,Samples.Num(),Mean,P95,Frozen.CompletedSteps,Frozen.PendingTicks);
                return Finish();
            }
            return true;
        }
    };
}

bool StartNativeWadSourceLightingReview(UWorld* World,const FString& Output,int32 Size,bool bReload)
{
    if(!World||!GEngine||!World->IsGameWorld()||!World->GetPackage()->GetName().Contains(TEXT("/SourceLightingCandidates/"))
        ||!World->URL.HasOption(TEXT("WadSourceLightingFixture"))||FParse::Param(FCommandLine::Get(),TEXT("nullrhi"))||Size<128||Size>2048){return false;}
    const auto* Context=GEngine->GetWorldContextFromWorld(World);if(!Context){return false;}
    auto Review=MakeShared<FSourceLightingReview>();Review->WorldHandle=Context->ContextHandle;Review->Output=Output;Review->Size=Size;Review->bReload=bReload;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float){return Review->Tick();}));return true;
}
