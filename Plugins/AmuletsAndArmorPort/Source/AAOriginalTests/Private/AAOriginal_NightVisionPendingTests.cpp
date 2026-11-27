#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Materials/Material.h"
#include "NativeWadNightVision.h"
#include "RHI.h"
#include "UObject/StrongObjectPtr.h"
#include "../../AAOriginal/Private/NativeWadShaderReadiness.h"

namespace
{
    class FPendingParentProof final : public IAutomationLatentCommand
    {
        TStrongObjectPtr<UMaterial> Parent;
        FAutomationTestBase* Test;
        int32 Phase=0;
        double Started=FPlatformTime::Seconds();
    public:
        FPendingParentProof(UMaterial* InParent,FAutomationTestBase* InTest):Parent(InParent),Test(InTest){}
        virtual bool Update() override
        {
            if(FPlatformTime::Seconds()-Started>60)
            {Test->AddError(TEXT("Timed out waiting for actual pending material proof"));return true;}
            if(Phase==0)
            {
                // Only this test explicitly completes shader work. The runtime
                // uses normal engine/core ticks and never waits for render work.
                if(!Test->TestTrue(TEXT("Transient parent compiles to its exact renderer resource"),NativeWadShaderReadiness::Prepare(*Parent))){return true;}
                Phase=1;return false;
            }
            if(!NativeWadNightVision::IsRenderingReady(*Parent)){return false;}
            if(Phase==1)
            {
                Test->TestTrue(TEXT("Cold parent becomes usable after async actual-resource proof"),true);
                const FGuid Generation=Parent->StateId;
                Parent->TwoSided=!Parent->TwoSided;
                Parent->PostEditChange(); // Genuine editor compilation, not a readiness flag.
                Test->TestTrue(TEXT("Actual material edit changes its generation"),Parent->StateId!=Generation);
                Test->TestFalse(TEXT("Hot compilation invalidates the previous proof immediately"),NativeWadNightVision::IsRenderingReady(*Parent));
                if(!Test->TestTrue(TEXT("Edited transient parent compiles without fallback"),NativeWadShaderReadiness::Prepare(*Parent))){return true;}
                Phase=2;return false;
            }
            Test->TestTrue(TEXT("Edited parent becomes usable only after a fresh actual-resource proof"),true);
            return true;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalNightVisionPendingLifecycle,"AAOriginal.WAD.Interface.NightVisionPendingLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalNightVisionPendingLifecycle::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender()||GUsingNullRHI)
    { AddInfo(TEXT("Actual shader/renderer transition requires RHI; PrivateViews still verifies pending owners/MIDs without fallback, and rendered review proves pixels."));return true; }
    auto* Saved=LoadObject<UMaterial>(nullptr,NativeWadNightVision::MaterialPath);
    if(!TestNotNull(TEXT("Saved owned vision material"),Saved)){return false;}
    UMaterial* Parent=DuplicateObject<UMaterial>(Saved,GetTransientPackage());
    if(!TestNotNull(TEXT("Transient independent parent for actual editor compilation"),Parent)){return false;}
    NativeWadNightVision::PrimeRendering(*Parent);
    TestFalse(TEXT("Cold parent stays pending until its actual render-thread proof returns"),NativeWadNightVision::IsRenderingReady(*Parent));
    ADD_LATENT_AUTOMATION_COMMAND(FPendingParentProof(Parent,this));
    return true;
}
