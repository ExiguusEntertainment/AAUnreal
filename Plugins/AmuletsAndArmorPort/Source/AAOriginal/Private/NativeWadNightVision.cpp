#include "NativeWadNightVision.h"

#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"
#include "Misc/App.h"
#include "RHI.h"
#include "Materials/Material.h"
#include "NativeWadShaderReadiness.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NativeWadHudData.h"
#include "NativeWadPlayer.h"

namespace
{
    struct FVisionParentState
    {
        FGuid Generation;
        FMaterialResource* Resource = nullptr;
        const FMaterialShaderMap* Map = nullptr;
        TSharedPtr<NativeWadShaderReadiness::FRenderCheck,ESPMode::ThreadSafe> Check;
        bool bReady = false;
        bool bTickerActive = false;
        bool bRestarted = false;
        bool bFailed = false;
    };
    TMap<TWeakObjectPtr<UMaterialInterface>,TSharedRef<FVisionParentState>> VisionParents;

    void RefreshEpoch(UMaterialInterface& Parent,FVisionParentState& State)
    {
        auto* Resource=Parent.GetMaterialResource(GMaxRHIFeatureLevel);
        const auto* Map=Resource?Resource->GetGameThreadShaderMap():nullptr;
        const FGuid Generation=Parent.GetMaterial()?Parent.GetMaterial()->StateId:FGuid();
        if (State.Resource!=Resource||State.Map!=Map||State.Generation!=Generation)
        {
            State.Generation=Generation;State.Resource=Resource;State.Map=Map;
            State.Check.Reset();State.bReady=false;State.bRestarted=false;State.bFailed=false;
        }
    }
    void StartPrime(UMaterialInterface& Parent,const TSharedRef<FVisionParentState>& State)
    {
        if (State->bTickerActive||State->bReady||State->bFailed||(!FApp::CanEverRender()||GUsingNullRHI)) { return; }
        State->bTickerActive=true;
        const TWeakObjectPtr<UMaterialInterface> WeakParent(&Parent);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakParent,State](float)
        {
            UMaterialInterface* Parent=WeakParent.Get();
            if (!Parent) { State->bTickerActive=false;return false; }
            RefreshEpoch(*Parent,*State);
            FMaterialResource* R=Parent->GetMaterialResource(GMaxRHIFeatureLevel);
#if WITH_EDITOR
            if (R)
            {
                // IsCompilationFinished checks async DDC readiness BEFORE it
                // completes cache work; it does not wait for pending DDC/jobs.
                R->IsCompilationFinished();
                R->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
                if (!R->GetCompileErrors().IsEmpty())
                {
                    for (const FString& Error:R->GetCompileErrors())
                    { UE_LOG(LogTemp,Error,TEXT("Pending NightVision material failed: %s"),*Error); }
                    State->bFailed=true;State->bTickerActive=false;return false;
                }
                if (!NativeWadShaderReadiness::IsGameThreadReady(*Parent)&&R->IsCompilationFinished()&&!State->bRestarted)
                {
                    // A missing map has no submitting ID. Start one async cache
                    // request only after previous DDC/work is already finished.
                    State->bRestarted=true;
                    R->BeginCacheShaders(GShaderPlatformForFeatureLevel[GMaxRHIFeatureLevel],EMaterialShaderPrecompileMode::Background);
                }
            }
#endif
            RefreshEpoch(*Parent,*State);
            if (!NativeWadShaderReadiness::IsGameThreadReady(*Parent))
            {
#if !WITH_EDITOR
                UE_LOG(LogTemp,Error,TEXT("Cooked NightVision parent has no complete shader map; effect remains pending without a fallback."));
                State->bFailed=true;State->bTickerActive=false;return false;
#else
                return true;
#endif
            }
            if (State->Check)
            {
                if (!State->Check->bDone.load(std::memory_order_acquire)) { return true; }
                if (NativeWadShaderReadiness::Passed(*State->Check))
                { State->bReady=true;State->bTickerActive=false;return false; }
                State->Check.Reset(); // A render-thread map update can lag a game-thread update.
            }
            State->Check=NativeWadShaderReadiness::QueueRenderCheck(*Parent);
            return true;
        }),0.01f);
    }
    TSharedRef<FVisionParentState> ParentState(UMaterialInterface& Parent)
    {
        for (auto It=VisionParents.CreateIterator();It;++It)
        { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
        const TWeakObjectPtr<UMaterialInterface> Key(&Parent);
        auto* Existing=VisionParents.Find(Key);
        if (!Existing) { Existing=&VisionParents.Add(Key,MakeShared<FVisionParentState>()); }
        RefreshEpoch(Parent,**Existing);
        if (!NativeWadShaderReadiness::IsGameThreadReady(Parent)) { (*Existing)->bReady=false; }
        else { (*Existing)->bFailed=false; } // An editor retry may recover the same generation.
        StartPrime(Parent,*Existing);
        return *Existing;
    }
}

void NativeWadNightVision::PrimeRendering(UMaterialInterface& Parent)
{
    if (Parent.GetMaterial()) { ParentState(Parent); }
}

bool NativeWadNightVision::IsRenderingReady(UMaterialInterface& Parent)
{
    if (!FApp::CanEverRender()||GUsingNullRHI) { return false; }
    return Parent.GetMaterial()&&ParentState(Parent)->bReady;
}

void NativeWadNightVision::MarkSky(UPrimitiveComponent& Component)
{
    // Constant classification shared by every viewer. No player strength lives here.
    Component.SetRenderCustomDepth(true);
    Component.SetCustomDepthStencilValue(SkyStencil);
}

int32 ANativeWadPlayer::GetNightVisionPower() const
{
    int32 Strongest = 0;
    for (const auto& Effect : TimedEffects.GetEffects())
    {
        if (Effect.Kind == EWadTimedEffectKind::Status && Effect.Status == NativeWadRules::NightVisionStatus && Effect.Seconds > 0.0)
        { Strongest = FMath::Max(Strongest, Effect.Power); }
    }
    return NativeWadNightVision::BoundPower(Strongest);
}

void ANativeWadPlayer::UpdateNightVisionRendering()
{
    if (!ViewCamera) { return; }
    const int32 Power = GetNightVisionPower();
    if (Power <= 0)
    {
        if (NightVisionMaterial)
        {
            NightVisionMaterial->SetScalarParameterValue(NativeWadNightVision::PowerParameter, 0.0f);
            ViewCamera->RemoveBlendable(TScriptInterface<IBlendableInterface>(NightVisionMaterial.Get()));
        }
        return;
    }
    const auto* Catalog = UNativeWadHudData::LoadCatalog();
    UMaterialInterface* Parent = Catalog ? Catalog->NightVisionPostProcess.Get() : nullptr;
    if (!Parent)
    {
        if (!bNightVisionAssetWarning)
        {
            bNightVisionAssetWarning = true;
            UE_LOG(LogTemp, Error, TEXT("Night vision has no saved camera postprocess parent. Run AA.ImportWadNightVision before review/cooking."));
        }
        return;
    }
    if (NightVisionMaterial && NightVisionMaterial->Parent != Parent)
    {
        ViewCamera->RemoveBlendable(TScriptInterface<IBlendableInterface>(NightVisionMaterial.Get()));
        NightVisionMaterial = nullptr;
    }
    if (!NightVisionMaterial) { NightVisionMaterial = UMaterialInstanceDynamic::Create(Parent, this); }
    NightVisionMaterial->SetScalarParameterValue(NativeWadNightVision::PowerParameter, float(Power));
    if (!NativeWadNightVision::IsRenderingReady(*Parent))
    {
        // Keep the desired owner/power and private MID, but never attach an
        // incomplete/fallback parent. No compilation or render flush here.
        ViewCamera->RemoveBlendable(TScriptInterface<IBlendableInterface>(NightVisionMaterial.Get()));
        return;
    }
    ViewCamera->AddOrUpdateBlendable(TScriptInterface<IBlendableInterface>(NightVisionMaterial.Get()), 1.0f);
    ViewCamera->SetPostProcessBlendWeight(1.0f);
}
