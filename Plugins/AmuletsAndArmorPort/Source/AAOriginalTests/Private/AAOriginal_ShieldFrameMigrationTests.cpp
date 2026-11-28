#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "NativeWadArt.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadScenery.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadImport.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalShieldFramePixels,"AAOriginal.WAD.Import.ShieldFrameSourceGuards",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalShieldFramePixels::RunTest(const FString& Parameters)
{
    auto* Old=LoadObject<UTexture2D>(nullptr,TEXT("/Game/WadInterface/Inventory/Things/T_SHIELD02_PLH02.T_SHIELD02_PLH02"));
    if (!TestNotNull(TEXT("Retained original generated legacy texture"),Old)) { return false; }
    TestTrue(TEXT("Actual old generated64x64 pixels and sampler defaults match sourcePLH02"),MatchesNativeWadShieldFrameTexture(*Old,true));
    auto* Copy=DuplicateObject<UTexture2D>(Old,GetTransientPackage());if (!Copy) { return false; }
    TArray64<uint8> Original;if (!Copy->Source.GetMipData(Original,0)) { return false; }
    auto Changed=Original;Changed[0]^=1;Copy->Source.Init(64,64,1,1,TSF_BGRA8,Changed.GetData());
    TestFalse(TEXT("Custom pixels are rejected after copying the generated source texture"),MatchesNativeWadShieldFrameTexture(*Copy,true));
    Copy->Source.Init(64,64,1,1,TSF_BGRA8,Original.GetData());Copy->Filter=TF_Bilinear;
    TestFalse(TEXT("Custom sampling settings are not silently replaced"),MatchesNativeWadShieldFrameTexture(*Copy,true));Copy->Filter=TF_Nearest;
    FWadArtProvenance Wrong;Wrong.Encoding=EWadArtEncoding::Sprite;Wrong.ResourcePath=TEXT("OBJS/SHIELD02/PLH01.CPC");Wrong.OriginalSize={64,64};Wrong.bZeroTransparent=true;
    Wrong.SourceSHA1=FString::ChrN(40,TEXT('0'));Wrong.MipSHA1.Add(NativeWadArt::SHA1(MakeArrayView(Original.GetData(),int32(Original.Num()))));
    TestTrue(TEXT("Install a structurally valid conflicting provenance record on the transient fixture"),NativeWadArt::RecordSource(*Copy,Wrong));
    TestFalse(TEXT("IdenticalRGB never overrides conflicting source identity"),MatchesNativeWadShieldFrameTexture(*Copy,true));
    auto* New=LoadObject<UTexture2D>(nullptr,TEXT("/Game/WadInterface/Inventory/Things/T_SHIELD02_PLH01.T_SHIELD02_PLH01"));
    if (!TestNotNull(TEXT("SourcePLH01 asset after the bounded migration"),New)) { return false; }
    TestTrue(TEXT("Corrected source art retains exact pixels, padding, domain and provenance"),MatchesNativeWadShieldFrameTexture(*New,false));
    TestFalse(TEXT("The two source frames are not treated as interchangeable"),MatchesNativeWadShieldFrameTexture(*New,true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalShieldFrameBindings,"AAOriginal.WAD.Import.ShieldFrameBindings",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalShieldFrameBindings::RunTest(const FString& Parameters)
{
    auto* World=UWorld::CreateWorld(EWorldType::Game,false);if (!World) { return false; }ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Old=NewObject<UMaterial>();auto* New=NewObject<UMaterial>();auto* Custom=NewObject<UMaterial>();
    auto* Level=World->SpawnActor<ANativeWadLevel>();auto* Pickup=World->SpawnActor<ANativeWadPickup>();
    auto* Unsupported=World->SpawnActor<ANativeWadPickup>();auto* Override=World->SpawnActor<ANativeWadPickup>();
    auto* Scenery=World->SpawnActor<ANativeWadScenery>();if (!Level || !Pickup || !Unsupported || !Override || !Scenery) { return false; }
    FWadSceneryDefinition Definition;Definition.Radius=20;Definition.bPassable=true;
    Definition.Frames.AddDefaulted_GetRef().Views.Add({Old,{64,64},false});Definition.Frames.AddDefaulted_GetRef().Views.Add({Custom,{64,64},false});
    Scenery->Configure(*Level,nullptr,Definition,0,123);
    Pickup->ConfigureAuthored(nullptr,Old,712,3,{64,64},20,true,Scenery);Pickup->Tags.Add(TEXT("Keep source identity"));Pickup->SetAccessoryData(0);
    Unsupported->Configure(nullptr,Old,17096); // Same base bits, but no one of the11 verified source records.
    Override->Configure(nullptr,Custom,4808);
    const FTransform Before=Pickup->GetActorTransform(),SceneryBefore=Scenery->GetActorTransform();
    const auto* ViewProperty=FindFProperty<FStructProperty>(ANativeWadScenery::StaticClass(),TEXT("Definition"));if (!ViewProperty) { return false; }
    TestEqual(TEXT("Real pickup component, scenery view metadata and visible scenery binding all migrate"),RebindNativeWadShieldFrameMaterials(*World,*Old,*New),3);
    TestTrue(TEXT("Actor transform remains exactly unchanged"),Pickup->GetActorTransform().Equals(Before,0.0));
    TestTrue(TEXT("Scenery transform remains exactly unchanged"),Scenery->GetActorTransform().Equals(SceneryBefore,0.0));
    TestEqual(TEXT("Source identity is untouched"),Pickup->Tags[0],FName(TEXT("Keep source identity")));
    TestTrue(TEXT("Recorded payload zero remains distinct"),Pickup->HasAccessoryData() && Pickup->GetAccessoryData()==0);
    const auto& After=*ViewProperty->ContainerPtrToValuePtr<FWadSceneryDefinition>(Scenery);
    TestTrue(TEXT("Saved view binding is corrected so BeginPlay cannot restore the wrong frame"),After.Frames[0].Views[0].Material==New);
    TestTrue(TEXT("A separate valid view override is preserved"),After.Frames[1].Views[0].Material==Custom);
    TestTrue(TEXT("An unverified full object identity is never flattened into the migration"),Unsupported->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0)==Old);
    TestTrue(TEXT("Existing custom material override survives"),Override->FindComponentByClass<UStaticMeshComponent>()->GetMaterial(0)==Custom);
    TestEqual(TEXT("Reapplying the binding migration is idempotent"),RebindNativeWadShieldFrameMaterials(*World,*Old,*New),0);
    return true;
}
