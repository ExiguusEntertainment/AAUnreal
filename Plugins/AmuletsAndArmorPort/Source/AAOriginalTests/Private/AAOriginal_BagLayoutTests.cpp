#include "Misc/AutomationTest.h"
#include "NativeWadBagLayout.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagSourceGeometry,"AAOriginal.WAD.Inventory.BagSourceGeometry",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagSourceGeometry::RunTest(const FString& Parameters)
{
    using B=FWadBagLayout;
    TestEqual(TEXT("Source width18/height15 still occupy one17x14 cell"),B::Footprint({18,15}),FIntPoint(1,1));
    TestEqual(TEXT("Source width19/height16 occupy two cells each"),B::Footprint({19,16}),FIntPoint(2,2));
    TestEqual(TEXT("Original natural bitmap centering includes one-pixel inset"),B::BitmapOffset({19,16}),FIntPoint(8,7));
    TestEqual(TEXT("Original center-hotspot placement yields stored2,3"),B::Anchor({265,73},{213,16},{35,29}),FIntPoint(2,3));
    B Layout;TArray<FWadBagItem> Items={{200,{32,32},1,false},{201,{32,32},1,false},{202,{32,32},1,false}};
    if(!TestTrue(TEXT("Source auto-store initializes all objects"),Layout.Initialize(Items,false))){return false;}
    const auto& P=Layout.GetPlacements();
    TestEqual(TEXT("First source object bottom-right X"),P[0].X,4);TestEqual(TEXT("First source object bottom-right Y"),P[0].Y,5);
    TestEqual(TEXT("Descending Y before decrementing X"),FIntPoint(P[1].X,P[1].Y),FIntPoint(4,2));
    TestEqual(TEXT("Next free column block"),FIntPoint(P[2].X,P[2].Y),FIntPoint(2,5));
    TestEqual(TEXT("Every footprint cell selects exact same original index"),Layout.At(0,5,7,Items),0);
    TestEqual(TEXT("An empty cell remains empty"),Layout.At(0,0,0,Items),INDEX_NONE);
    TestEqual(TEXT("Always five original player bags, not dynamic slot pages"),Layout.PageCount(),5);
    B Creation;TestTrue(TEXT("Creation reorder initializes source row-major layout"),Creation.Initialize(Items,false,true));
    TestEqual(TEXT("Creation starts top-left"),FIntPoint(Creation.GetPlacements()[0].X,Creation.GetPlacements()[0].Y),FIntPoint(0,0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagStacksAndOwners,"AAOriginal.WAD.Inventory.BagStacksAndOwners",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagStacksAndOwners::RunTest(const FString& Parameters)
{
    FWadBagLayout L;TArray<FWadBagItem> Items;
    for(int32 I=0;I<6;++I){Items.Add({I==3?435+4096:435,{7,6},3,true});L.AddSlot();}
    const auto Place=[&](int32 Slot,FIntPoint A)
    {Items[Slot].bOffPage=false;const bool OK=L.Place(Slot,0,A,Items);if(!OK){Items[Slot].bOffPage=true;}return OK;};
    TestTrue(TEXT("Place first physical object"),Place(0,{0,0}));
    TestTrue(TEXT("Same type can form a separate manually positioned stack"),Place(1,{2,0}));
    TestEqual(TEXT("Separate stack is not automatically regrouped"),L.LeaderOf(1),1);
    TestTrue(TEXT("Exact same type merges onto occupied original stack"),Place(2,{0,0}));
    TestEqual(TEXT("Merged owner count"),L.StackCount(0),2);
    TestFalse(TEXT("Packed color variant refuses overlap instead of merging"),Place(3,{0,0}));
    TestEqual(TEXT("Rejected hand keeps its independent object identity"),L.LeaderOf(3),INDEX_NONE);
    TestTrue(TEXT("One more object fills source stack limit"),Place(4,{0,0}));
    TestFalse(TEXT("Full source stack refuses, without swapping objects"),Place(5,{0,0}));
    TestTrue(TEXT("Bag/held ownership invariant"),L.Validate(Items));
    const FString Original=L.Encode(Items);FWadBagLayout Restored;
    TestTrue(TEXT("Individual stack membership roundtrips"),Restored.Decode(Original,Items));
    TestEqual(TEXT("Separate manual stack retains leader"),Restored.LeaderOf(1),1);
    L.RemoveAt(0);Items.RemoveAt(0);
    TestTrue(TEXT("Removing a leader promotes surviving same stack while preserving every index remap"),L.Validate(Items));
    TestEqual(TEXT("Other manual stack index remaps once"),L.LeaderOf(0),0);
    TestEqual(TEXT("Promoted leader index"),L.LeaderOf(1),1);
    TestEqual(TEXT("Promoted stack stays at its original anchor"),FIntPoint(L.GetPlacements()[1].X,L.GetPlacements()[1].Y),FIntPoint(0,0));
    L.Detach(1);Items[1].bOffPage=true;
    TestTrue(TEXT("Taking one stack object leaves remaining objects and located ownership valid"),L.Validate(Items));
    TestEqual(TEXT("One detached hand object"),L.LeaderOf(1),INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagCapacityAndCodec,"AAOriginal.WAD.Inventory.BagCapacityAndCodec",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagCapacityAndCodec::RunTest(const FString& Parameters)
{
    FWadBagLayout L;TArray<FWadBagItem> Items;
    for(int32 I=0;I<240;++I){Items.Add({1000+I,{17,14},1,false});}
    TestTrue(TEXT("Exactly240 one-cell stacks fill five bags"),L.Initialize(Items,false));
    Items.Add({3000,{17,14},1,false});L.AddSlot();
    TestFalse(TEXT("New game cannot add a sixth page"),L.AutoPlace(240,Items));
    TestEqual(TEXT("Refusal preserves five-page capacity"),L.PageCount(),5);
    Items[240].bOffPage=true;TestTrue(TEXT("Full bags retain an independent mouse hand"),L.Validate(Items));
    Items[240].bOffPage=false;
    FWadBagLayout Legacy;TestTrue(TEXT("Old native ownership migrates without loss using explicit overflow"),Legacy.Initialize(Items,true));
    TestEqual(TEXT("Only required legacy extra page is retained"),Legacy.PageCount(),6);
    TestTrue(TEXT("Legacy capacity exception is explicit"),Legacy.HasLegacyOverflow());
    const FString Saved=Legacy.Encode(Items);FWadBagLayout Reload;
    TestTrue(TEXT("Explicit overflow survives strict codec"),Reload.Decode(Saved,Items));
    Reload.RemoveAt(240);Items.RemoveAt(240);
    TestEqual(TEXT("Empty legacy overflow is retired; no permanent unlimited bags"),Reload.PageCount(),5);
    TestFalse(TEXT("Legacy marker retires with overflow"),Reload.HasLegacyOverflow());
    TestTrue(TEXT("Remaining items retain valid placements"),Reload.Validate(Items));
    TArray<FWadBagItem> One={{435,{7,6},1,false}};FWadBagLayout Tiny;Tiny.Initialize(One,false);
    const FString Before=Tiny.Encode(One);
    for(const TCHAR* Invalid:{TEXT("1,5,0,1~435,0,6,0,0,0"),TEXT("1,5,0,1~435,0,0,8,0,0"),TEXT("1,5,0,1~435,0,0,0,1,0"),
        TEXT("1,5,0,1~4531,0,0,0,0,0"),TEXT("1,5,0,1~435,0,0,0,0junk,0"),TEXT("1,6,0,1~435,0,0,0,0,0"),TEXT("1,6,1,1~435,0,0,0,0,0")})
    {
        TestFalse(TEXT("Malformed/type-mismatched/capacity-inventing saved placement refuses"),Tiny.Decode(Invalid,One));
        TestEqual(TEXT("Codec failure is atomic and loses no owned placement"),Tiny.Encode(One),Before);
    }
    FWadBagLayout Empty;TestTrue(TEXT("Explicit empty modern layout remains distinguishable from missing old field"),Empty.Decode(TEXT("1,5,0,0"),{}));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagEdgesAndTraversal,"AAOriginal.WAD.Inventory.BagEdgesAndTraversal",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagEdgesAndTraversal::RunTest(const FString& Parameters)
{
    using B=FWadBagLayout;
    TestEqual(TEXT("Source truncation snaps small negative centered-bitmap numerator into cell0"),
        B::Anchor({213,16},{213,16},{19,16}),FIntPoint(0,0));
    TestEqual(TEXT("Source snapping places final bitmap inside bag despite cursor hotspot at border"),
        FIntPoint(213,16)+B::BitmapOffset({19,16}),FIntPoint(221,23));
    const FIntPoint Negative=B::Anchor({213,16},{213,16},{35,29});
    TestEqual(TEXT("Larger negative numerator remains negative grid anchor"),Negative,FIntPoint(-1,-1));
    B Edge;TArray<FWadBagItem> Single={{200,{35,29},1,true}};Edge.Initialize(Single,false);
    Single[0].bOffPage=false;
    TestFalse(TEXT("Negative grid anchors refuse without unsigned placement wrap"),Edge.Place(0,0,Negative,Single));
    TestEqual(TEXT("Refused placement retains detached object"),Edge.LeaderOf(0),INDEX_NONE);
    B L;TArray<FWadBagItem> Items;
    for(int32 I=0;I<5;++I){Items.Add({123,{19,16},5,true});L.AddSlot();}
    const auto Place=[&](int32 Slot,FIntPoint P){Items[Slot].bOffPage=false;return L.Place(Slot,0,P,Items);};
    TestTrue(TEXT("First source record at left"),Place(0,{0,0}));
    TestTrue(TEXT("Second separate source record at right"),Place(1,{2,0}));
    TestTrue(TEXT("Stack member of original first record"),Place(2,{0,0}));
    L.Detach(0);Items[0].bOffPage=true;
    TestEqual(TEXT("Native leader promotion changes index but not source record order"),L.LeaderOf(2),2);
    TestTrue(TEXT("Large held footprint spans both occupied footprints"),Place(3,{1,0}));
    TestEqual(TEXT("First original intersected record wins, despite its higher native leader index"),L.LeaderOf(3),2);
    const auto Order=L.LeadersInOrder();
    TestTrue(TEXT("Source draw/traversal order survives leader promotion"),Order.Num()==2&&Order[0]==2&&Order[1]==1);
    TestTrue(TEXT("Split hand, two manual stacks and stable record order validate"),L.Validate(Items));
    const FString Saved=L.Encode(Items);B Restore;
    TestTrue(TEXT("Record order persists independently of native indices"),Restore.Decode(Saved,Items));
    TestTrue(TEXT("Reload retains first-overlap order"),Restore.LeadersInOrder()==Order);
    FString Duplicate=Saved.Replace(TEXT("~123,0,0,0,2,0"),TEXT("~123,0,0,0,2,1"));
    TestFalse(TEXT("Duplicate canonical source traversal order rejects"),Restore.Decode(Duplicate,Items));
    TestEqual(TEXT("Failed order decode is atomic"),Restore.Encode(Items),Saved);
    return true;
}
