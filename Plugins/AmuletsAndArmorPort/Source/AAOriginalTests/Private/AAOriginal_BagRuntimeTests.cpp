#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadPlayer.h"
#include "NativeWadStockLayout.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadInventoryView.h"

namespace
{
    struct FBagRuntimeFixture
    {
        UGameInstance* Instance=nullptr;UWorld* World=nullptr;ANativeWadPlayer* Player=nullptr;APlayerController* Controller=nullptr;
        explicit FBagRuntimeFixture(const TCHAR* Options)
        {
            Instance=NewObject<UGameInstance>(GEngine);Instance->InitializeStandalone();World=Instance->GetWorld();if(!World){return;}
            World->URL=FURL(nullptr,*FString::Printf(TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode%s"),Options),TRAVEL_Absolute);
            if(!World->SetGameMode(World->URL)){return;}Controller=World->SpawnActor<APlayerController>();
            if(!Controller){return;}World->AddController(Controller);SpawnPlayer();
        }
        void SpawnPlayer()
        {Player=World->SpawnActor<ANativeWadPlayer>();if(Player){Controller->Possess(Player);Player->DispatchBeginPlay();}}
        ~FBagRuntimeFixture()
        {if(Instance){Instance->Shutdown();}if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}}
        FVector2D CenterFor(int32 Slot,FIntPoint Anchor) const
        {
            const auto Items=Player->GetBagItems();const auto Size=Items[Slot].Bitmap;
            return {213.0+Anchor.X*17+Size.X/2+1,16.0+Anchor.Y*14+Size.Y/2+1};
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagNativeCheckpoint,"AAOriginal.WAD.Inventory.BagNativeCheckpoint",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagNativeCheckpoint::RunTest(const FString& Parameters)
{
    FBagRuntimeFixture F(TEXT("?WadClass=1?WadCreated=1?WadRuneRules=1?WadItems=435,435,806,807,201,300?WadReady=4?WadJewelry=-1,0,-1,-1,-1?WadMouseHand=2"));
    if(!TestNotNull(TEXT("Native character with held, ready, worn and bag ownership"),F.Player)){return false;}
    auto& P=*F.Player;const auto Before=P.GetCarriedItems();
    TestEqual(TEXT("Source five bags on old-save migration"),P.GetBagLayout().PageCount(),5);
    TestFalse(TEXT("Cursor outside source left edge refuses"),P.TryPlaceMouseHandInBag(0,{212,16}));
    TestEqual(TEXT("Refusal retains exact held index"),P.GetMouseHandSlot(),2);
    TestTrue(TEXT("Normal pixel-coordinate placement stores the held potion"),P.TryPlaceMouseHandInBag(0,F.CenterFor(2,{0,0})));
    P.ClickInventorySlot(3);TestEqual(TEXT("Take keeps native source object identity"),P.GetMouseHandSlot(),3);
    TestTrue(TEXT("Normal source placement moves object to another bag"),P.TryPlaceMouseHandInBag(3,F.CenterFor(3,{2,2})));
    TestEqual(TEXT("Whole footprint cells resolve to exact placed item"),P.BagItemAt(3,{213+2*17+1,16+2*14+1}),3);
    P.SetBagPage(3);P.ClickInventorySlot(1);
    TestTrue(TEXT("Ready swap retains inventory ownership without a bag copy"),P.SwapMouseAndReady());
    TestEqual(TEXT("The old readied mace moves into the retained hand"),P.GetMouseHandSlot(),4);
    TestTrue(TEXT("Swap back restores the original ready index"),P.SwapMouseAndReady());
    TestTrue(TEXT("Put independent ring in bag2"),P.TryPlaceMouseHandInBag(2,F.CenterFor(1,{1,1})));
    P.ClickInventorySlot(2);
    TestTrue(TEXT("All physical locations validate before save"),P.GetBagLayout().Validate(P.GetBagItems()));
    const FString Layout=P.GetBagLayout().Encode(P.GetBagItems());
    constexpr const TCHAR* Slot=TEXT("NativeWadBagLayoutUnit");
    ON_SCOPE_EXIT{UGameplayStatics::DeleteGameInSlot(Slot,0);};
    if(!TestTrue(TEXT("Actual native checkpoint writes spatial inventory"),P.SaveWadCheckpoint(Slot))){return false;}
    const auto* Saved=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    if(!TestNotNull(TEXT("Actual saved checkpoint on disk"),Saved)){return false;}
    TestTrue(TEXT("Production travel/save writer contains additive layout field"),Saved->PlayerOptions.Contains(TEXT("?WadBagLayout=")));
    P.Destroy();F.World->URL.AddOption(*FString::Printf(TEXT("WadLoad=%s"),Slot));F.SpawnPlayer();
    F.Player->Tick(0.0f);
    TestTrue(TEXT("Actual native checkpoint restore executed"),F.Player->HasRestoredWadCheckpoint());
    TestTrue(TEXT("CarriedItems order and every integer identity survives disk save/actor restore"),F.Player->GetCarriedItems()==Before);
    TestEqual(TEXT("Spatial coordinates, manual groups and record traversal survive actual checkpoint"),F.Player->GetBagLayout().Encode(F.Player->GetBagItems()),Layout);
    TestEqual(TEXT("Character bag page survives native save"),F.Player->GetBagPage(),3);
    TestEqual(TEXT("Ready index survived"),F.Player->GetReadySlot(),4);TestEqual(TEXT("Held index survived"),F.Player->GetMouseHandSlot(),2);
    TestEqual(TEXT("Worn ring index survived"),F.Player->GetJewelryInventorySlot(NativeWadRules::Ring1Slot),0);
    // Same-process fresh-actor disk restore. The rendered retained-hand review separately tests process restart.
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagNativeCapacity,"AAOriginal.WAD.Inventory.BagNativeCapacity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagNativeCapacity::RunTest(const FString& Parameters)
{
    const auto* Catalog=static_cast<const UNativeWadHudData*>(LoadObject<UObject>(nullptr,UNativeWadHudData::ObjectPath));if(!TestNotNull(TEXT("Original bitmap catalog"),Catalog)){return false;}
    FBagRuntimeFixture F(TEXT("?WadClass=1?WadCreated=1?WadRuneRules=1?WadItems=?WadReady=-1"));if(!F.Player){return false;}
    const auto* Image=Catalog->Items.Find(712);
    if(!TestTrue(TEXT("Original shield picture is available"),Image&&*Image)){return false;}
    const FIntPoint Size=Catalog->ItemDetails.FindChecked(712).SpriteSize;
    TestEqual(TEXT("Runtime source dimensions stay independent of texture compilation"),Catalog->GetItemBitmapSize(712),Size);
    TestEqual(TEXT("Source64x64 shield is4x5 cells, not a one-cell thumbnail"),FWadBagLayout::Footprint(Size),FIntPoint(4,5));
    for(int32 I=0;I<5;++I){TestTrue(TEXT("One original large shield fits each source bag"),F.Player->CollectItem(712));}
    TestEqual(TEXT("Player capacity uses saved original bitmap metadata"),F.Player->GetBagItems()[0].Bitmap,Size);
    TestFalse(TEXT("A sixth large item refuses instead of creating unlimited pages"),F.Player->CollectItem(712));
    TestEqual(TEXT("Failed collection grants no extra physical object"),F.Player->GetItemCount(712),5);
    TestEqual(TEXT("New game has exactly five pages"),F.Player->GetBagLayout().PageCount(),5);
    TestFalse(TEXT("New game never acquires legacy exception from collection"),F.Player->GetBagLayout().HasLegacyOverflow());
    TestTrue(TEXT("Take one shield through the normal inventory entry point"),F.Player->TakeInventoryItem(0));
    const auto Locations=F.Player->GetBagLayout().Encode(F.Player->GetBagItems());
    TestFalse(TEXT("Large item's negative centered grid anchor refuses"),F.Player->TryPlaceMouseHandInBag(0,{213,16}));
    TestEqual(TEXT("Negative-grid refusal changes no location"),F.Player->GetBagLayout().Encode(F.Player->GetBagItems()),Locations);
    TestTrue(TEXT("The vacated source footprint accepts that exact object"),F.Player->TryPlaceMouseHandInBag(0,F.CenterFor(0,{0,0})));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMerchantFootprints,"AAOriginal.WAD.Inventory.MerchantFootprints",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalMerchantFootprints::RunTest(const FString& Parameters)
{
    TArray<FWadStockItem> Items={{201,0,{19,16}},{202,0,{19,16}},{435,3,{7,6}}};
    FWadStockLayout Store,Bank;
    TestTrue(TEXT("Source Store6x7 packs natural bitmap footprints"),Store.Build(Items,7));
    TestTrue(TEXT("Source Bank6x3 packs natural bitmap footprints"),Bank.Build(Items,3));
    TestEqual(TEXT("Any cell within large stock footprint selects the same source type"),Store.At(0,{1,1}),201);
    TestEqual(TEXT("Second stock object starts after occupied width"),Store.At(0,{2,0}),202);
    TestEqual(TEXT("Ring category begins its own source page"),Store.CategoryAt(1),3);
    TestEqual(TEXT("Separate category page hit target"),Store.At(1,{0,0}),435);
    TestFalse(TEXT("Stock rendering cannot silently shrink a too-tall item into Bank's three rows"),Bank.Build(TArray<FWadStockItem>{{712,1,{64,64}}},3));
    const auto* Catalog=static_cast<const UNativeWadHudData*>(LoadObject<UObject>(nullptr,UNativeWadHudData::ObjectPath));if(!Catalog){return false;}
    for(const auto Pair:TArray<FIntPoint>{{201,0},{243,7},{435,3},{806,4},{712,1}})
    {
        const auto* Rule=Catalog->ItemDetails.Find(Pair.X);if(!TestNotNull(TEXT("Saved original item category"),Rule)){return false;}
        TestEqual(TEXT("Merchant grouping uses imported original EQUIP category, not numeric-ID heuristics"),Rule->SourceCategory,Pair.Y);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagSourceBitmaps,"AAOriginal.WAD.Inventory.BagSourceBitmaps",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagSourceBitmaps::RunTest(const FString& Parameters)
{
    const auto* Catalog=static_cast<const UNativeWadHudData*>(LoadObject<UObject>(nullptr,UNativeWadHudData::ObjectPath));
    if(!TestNotNull(TEXT("Imported original inventory catalog"),Catalog)){return false;}
    TestTrue(TEXT("Full imported inventory catalog is checked"),Catalog->Items.Num()>=375);
    for(const auto& Pair:Catalog->Items)
    {
        if(!Pair.Value){AddError(FString::Printf(TEXT("Missing original bitmap for%d"),Pair.Key));continue;}
        const FIntPoint Source(int32(Pair.Value->Source.GetSizeX()),int32(Pair.Value->Source.GetSizeY()));
        const auto* Details=Catalog->ItemDetails.Find(Pair.Key);
        TestTrue(FString::Printf(TEXT("Original inventory source dimensions exist for%d"),Pair.Key),Source.X>0&&Source.Y>0);
        TestTrue(FString::Printf(TEXT("Saved metadata equals inventory source bitmap for%d"),Pair.Key),Details&&Details->SpriteSize==Source);
        TestEqual(FString::Printf(TEXT("Stable runtime bitmap metadata for%d"),Pair.Key),Catalog->GetItemBitmapSize(Pair.Key),Source);
    }
    return true;
}
