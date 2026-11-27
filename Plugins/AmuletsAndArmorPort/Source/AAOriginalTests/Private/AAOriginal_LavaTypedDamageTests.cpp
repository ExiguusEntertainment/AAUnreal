#include "Misc/AutomationTest.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FLavaDamageFixture
    {
        UGameInstance* Instance=NewObject<UGameInstance>(GEngine);
        UWorld* World=nullptr;
        ANativeWadPlayer* Player=nullptr;
        explicit FLavaDamageFixture(const TCHAR* Effect=nullptr)
        {
            Instance->InitializeStandalone();World=Instance->GetWorld();if(!World){return;}
            World->URL=FURL(nullptr,TEXT("/Game/Maps/L40?WadClass=4?WadCreated=1?WadLevel=100?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=30000?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadItems=?WadReady=-1"),TRAVEL_Absolute);
            if(Effect){World->URL.AddOption(*(TEXT("WadOwnedEffects=")+FString(Effect)));}
            auto* Level=World->SpawnActor<ANativeWadLevel>();
            const auto* Property=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
            if(!Level||!Property){return;}
            auto& Sectors=*Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
            auto& Sector=Sectors.AddDefaulted_GetRef();Sector.bLava=true;Sector.FloorDepth=20;
            Sector.RepairedBoundaryPoints={{-500,-500},{500,-500},{500,-500},{500,500},{500,500},{-500,500},{-500,500},{-500,-500}};
            const auto Surface=[&](double Z)
            {
                auto* C=NewObject<UStaticMeshComponent>(Level);C->SetupAttachment(Level->GetRootComponent());
                C->SetMobility(EComponentMobility::Movable);C->SetRelativeLocation(FVector(0,0,Z));C->RegisterComponent();return C;
            };
            Sector.Floor=Surface(0);Sector.Ceiling=Surface(2000);Sector.WalkingFloor=Surface(-20);Level->SetSectorFloorZ(0,0);
            Player=World->SpawnActor<ANativeWadPlayer>(FVector(0,0,ANativeWadPlayer::CapsuleHalfHeight-20),FRotator::ZeroRotator);
            auto* Controller=World->SpawnActor<APlayerController>();if(!Player||!Controller){return;}
            World->AddController(Controller);Controller->Possess(Player);Player->DispatchBeginPlay();
        }
        ~FLavaDamageFixture(){Instance->Shutdown();if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}}
        int32 Pulse()const{const int32 Before=Player->GetHealth();Player->Tick(0.51f);return Before-Player->GetHealth();}
        bool Wear(int32 Type,int32 Location)const
        {
            int32 Slot=Player->GetCarriedItems().IndexOfByKey(Type);
            if(Slot==INDEX_NONE){if(!Player->CollectItem(Type)){return false;}Slot=Player->GetCarriedItems().Num()-1;}
            if(!Player->TakeInventoryItem(Slot)){return false;}Player->ClickJewelrySlot(Location);return Player->HasEquippedItem(Type);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalLavaTypedDamage,"AAOriginal.WAD.Gameplay.LavaTypedDamage",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalLavaTypedDamage::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FLavaDamageFixture Fixture;if(!Fixture.Player){return false;}
    TestEqual(TEXT("Source raw200 Normal|Fire includes the original unresisted fire bonus"),Fixture.Pulse(),250);
    if(!Fixture.Wear(607,NeckSlot)){return false;}
    TestEqual(TEXT("Actual source fire resistance removes the bonus but leaves the normal half"),Fixture.Pulse(),100);
    Fixture.Player->ClickJewelrySlot(NeckSlot);Fixture.Player->StoreMouseHandInInventory();
    if(!Fixture.Wear(415,Ring1Slot)){return false;}
    TestEqual(TEXT("Original magical shielding armor5 is retained"),Fixture.Player->GetArmorRating(),5);
    TestEqual(TEXT("The fire bonus precedes ordinary source armor mitigation"),Fixture.Pulse(),245);
    if(!Fixture.Wear(607,NeckSlot)){return false;}
    TestEqual(TEXT("Resistance precedes armor on the mixed lava hit"),Fixture.Pulse(),98);
    const FString LavaWalk=FString::Printf(TEXT("1~spell:92,%d,%d,1,30"),int32(EWadTimedEffectKind::Status),int32(LavaWalkStatus));
    FLavaDamageFixture Walker(*LavaWalk);if(!Walker.Player){return false;}
    TestEqual(TEXT("Existing source LavaWalk exemption remains intact"),Walker.Pulse(),0);
    return true;
}
