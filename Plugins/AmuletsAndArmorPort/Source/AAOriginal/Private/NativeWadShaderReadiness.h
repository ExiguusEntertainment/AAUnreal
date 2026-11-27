#pragma once
#include "CoreMinimal.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "RenderingThread.h"
#include "Materials/MaterialRenderProxy.h"
#include <atomic>
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

namespace NativeWadShaderReadiness
{
    inline bool IsGameThreadReady(UMaterialInterface& Material)
    {
        const FMaterialResource* R=Material.GetMaterialResource(GMaxRHIFeatureLevel);
        if (!R||!R->GetGameThreadShaderMap()||!R->IsGameThreadShaderMapComplete()) { return false; }
#if WITH_EDITOR
        if (!R->GetCompileErrors().IsEmpty()) { return false; }
#endif
        return true;
    }
    struct FRenderCheck
    {
        std::atomic<bool> bDone{false};
        bool bExactResource = false;
        bool bComplete = false;
        bool bFallback = true;
    };
    inline TSharedRef<FRenderCheck,ESPMode::ThreadSafe> QueueRenderCheck(UMaterialInterface& Material)
    {
        const auto Result=MakeShared<FRenderCheck,ESPMode::ThreadSafe>();
        FMaterialResource* Expected=Material.GetMaterialResource(GMaxRHIFeatureLevel);
        FMaterialRenderProxy* Proxy=Material.GetRenderProxy();
        if (!Expected||!Proxy) { Result->bDone.store(true,std::memory_order_release);return Result; }
        TRefCountPtr<FMaterial> Retained(Expected);
        const auto Feature=GMaxRHIFeatureLevel;
        ENQUEUE_RENDER_COMMAND(WadNightVisionCheckActualResource)([Result,Retained,Proxy,Feature](FRHICommandListImmediate&)
        {
            const FMaterialRenderProxy* Fallback=nullptr;
            const FMaterial& Actual=Proxy->GetMaterialWithFallback(Feature,Fallback);
            Result->bExactResource=&Actual==Retained.GetReference();
            Result->bComplete=Actual.IsRenderingThreadShaderMapComplete();
            Result->bFallback=Fallback!=nullptr;
            Result->bDone.store(true,std::memory_order_release);
        });
        return Result;
    }
    inline bool Passed(const FRenderCheck& Result)
    {
        return Result.bDone.load(std::memory_order_acquire)&&Result.bExactResource&&Result.bComplete&&!Result.bFallback;
    }
    inline bool IsReady(UMaterialInterface& Material)
    {
        if (!IsGameThreadReady(Material)) { return false; }
        const auto Result=QueueRenderCheck(Material);
        // Import/render-review only. Runtime polls QueueRenderCheck without a flush.
        FlushRenderingCommands();
        const bool Ready=Passed(*Result);
        UE_LOG(LogTemp,Display,TEXT("NightVision actual render resource: exact%d complete%d fallback%d ready%d legacyFinalizedValid%d"),
            Result->bExactResource,Result->bComplete,Result->bFallback,Ready,Material.GetMaterialResource(GMaxRHIFeatureLevel)->HasValidGameThreadShaderMap());
        return Ready;
    }
    inline void Describe(UMaterialInterface& Material,const TCHAR* Stage)
    {
        const FMaterialResource* R=Material.GetMaterialResource(GMaxRHIFeatureLevel);
        UE_LOG(LogTemp,Display,TEXT("NightVision shader %s %s base%s resource%p requestedFeature%d ready%d"),
            Stage,*Material.GetPathName(),*Material.GetMaterial()->GetPathName(),R,int32(GMaxRHIFeatureLevel),IsReady(Material));
        if (!R) { return; }
#if WITH_EDITOR
        UE_LOG(LogTemp,Display,TEXT("NightVision shader detail: feature%d quality%d map%p valid%d flagComplete%d cachePending%d errors%d"),
            int32(R->GetFeatureLevel()),int32(R->GetQualityLevel()),R->GetGameThreadShaderMap(),R->HasValidGameThreadShaderMap(),
            R->IsGameThreadShaderMapComplete(),R->IsCachingShaders(),R->GetCompileErrors().Num());
        for (const FString& Error:R->GetCompileErrors()) { UE_LOG(LogTemp,Error,TEXT("NightVision shader compile error: %s"),*Error); }
        if (FMaterialShaderMap* Map=R->GetGameThreadShaderMap())
        {
            // IsComplete itself is not exported across the Engine DLL in UE5.6.
            // Keep the public resource predicate and log safe inline map metadata.
            int32 Shaders=0,Pipelines=0;Map->CountNumShaders(Shaders,Pipelines);
            UE_LOG(LogTemp,Display,TEXT("NightVision shader map finalized%d platform%d shaders%d pipelines%d"),
                Map->IsCompilationFinalized(),int32(Map->GetShaderPlatform()),Shaders,Pipelines);
        }
#endif
    }
    inline bool Prepare(UMaterialInterface& Material)
    {
        Describe(Material,TEXT("before cache completion"));
#if WITH_EDITOR
        if (FMaterialResource* R=Material.GetMaterialResource(GMaxRHIFeatureLevel))
        {
            // EnsureIsComplete submits before it finishes pending DDC cache work.
            // Finish that work FIRST, so the on-demand compiling map/ID exists.
            R->FinishCacheShaders();
            R->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::ForceLocal);
            R->FinishCompilation();
        }
        FlushRenderingCommands();
        Describe(Material,TEXT("after ordered submit/finish"));
        if (!IsReady(Material))
        {
            // Public editor API starts missing maps/permutations synchronously;
            // never set completion flags or accept fallback shaders as evidence.
            if (UMaterial* Base=Material.GetMaterial())
            {
                FMaterialUpdateContext Context;
                Context.AddMaterial(Base);
                Base->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
                Base->EnsureIsComplete();
            }
            FlushRenderingCommands();
            Describe(Material,TEXT("after synchronous cache request"));
        }
#endif
        return IsReady(Material);
    }
}
