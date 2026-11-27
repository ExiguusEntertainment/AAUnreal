#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadMagicMap.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "NativeWadLevel.h"
#include "NativeWadScenery.h"
#include "NativeWadEncounter.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"

namespace
{
    struct FMapFixture
    {
        UGameInstance* Instance=nullptr;UWorld* World=nullptr;ANativeWadPlayer* Player=nullptr;UInputComponent* Input=nullptr;ANativeWadLevel* Level=nullptr;
        FMapFixture(int32 Class,int32 Mana=30000,const FString& Restore=FString(),bool bScene=false)
        {
            Instance=NewObject<UGameInstance>(GEngine);Instance->InitializeStandalone();World=Instance->GetWorld();if(!World){return;}
            FString Options=FString::Printf(TEXT("/Game/Maps/L30?WadClass=%d?WadCreated=1?WadRuneRules=1?WadLevel=100")
                TEXT("?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=10000?WadMaxMana=30000?WadMana=%d?WadFood=0?WadWater=0?WadReady=-1?WadItems="),Class,Mana);
            for(int32 Rune=1;Rune<=9;++Rune){if(Rune>1){Options+=TEXT(",");}Options+=FString::FromInt(NativeWadRules::RuneItemForClass(Class,Rune));}
            if(!Restore.IsEmpty()){Options=TEXT("/Game/Maps/L30?")+Restore;}
            World->URL=FURL(nullptr,*Options,TRAVEL_Absolute);
            if(bScene){Level=World->SpawnActor<ANativeWadLevel>();}
            Player=World->SpawnActor<ANativeWadPlayer>();
            auto* PC=World->SpawnActor<APlayerController>();if(!Player||!PC){return;}World->AddController(PC);PC->Possess(Player);
            Player->DispatchBeginPlay();Input=NewObject<UInputComponent>(Player);Player->SetupPlayerInputComponent(Input);
        }
        ~FMapFixture(){if(Instance){Instance->Shutdown();}if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}}
        bool Press(FKey Key) const
        {
            if(!Input){return false;}for(const auto& Binding:Input->KeyBindings)
            {if(Binding.KeyEvent==IE_Pressed&&Binding.Chord==FInputChord(Key)){Binding.KeyDelegate.Execute(Key);return true;}}return false;
        }
        bool Cast(const TCHAR* Code) const
        {
            const FKey Keys[]={EKeys::NumPadOne,EKeys::NumPadTwo,EKeys::NumPadThree,EKeys::NumPadFour,EKeys::NumPadFive,
                EKeys::NumPadSix,EKeys::NumPadSeven,EKeys::NumPadEight,EKeys::NumPadNine};
            for(;*Code;++Code){if(*Code<TEXT('1')||*Code>TEXT('9')||!Press(Keys[*Code-TEXT('1')])){return false;}}
            return Press(EKeys::RightMouseButton);
        }
    };
    FWadMagicMap::FLine Wall(double X,double Bottom=0,double Top=100,EWadWallBlend Blend=EWadWallBlend::Opaque)
    {
        FWadMagicMap::FLine L;L.Wall.From={X,-100};L.Wall.To={X,100};
        FWadWallSurface S;S.Bottom=Bottom;S.Top=Top;S.Blend=Blend;S.bBack=true;L.Wall.Surfaces.Add(S);return L;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMagicMapRunes,"AAOriginal.WAD.Gameplay.MagicMapRunes",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalMagicMapRunes::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const int32 Definitions[]={13,34,43,44,54,68,80,100};
    for(int32 Definition:Definitions)
    {
        const auto* Spell=Algo::FindByPredicate(SupportedSpells,[&](const auto& S){return S.SourceDefinition==Definition;});
        if(!TestNotNull(TEXT("Original map definition exists"),Spell)){return false;}
        TArray<FString> Raw,Lines;
        if(!TestTrue(TEXT("Read source spell"),FFileHelper::LoadFileToStringArray(Raw,*(FPaths::ProjectDir()/FString::Printf(
            TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC/SPL%05d.TXT"),Definition))))){return false;}
        for(const auto& Row:Raw){const FString S=Row.TrimStartAndEnd();if(!S.IsEmpty()&&!S.StartsWith(TEXT("#"))){Lines.Add(S);}}
        const auto Field=[&](const TCHAR* Name){const int32 I=Lines.Find(Name);return Lines.IsValidIndex(I+1)&&I!=INDEX_NONE?Lines[I+1]:FString();};
        FString Runes=Field(TEXT("ACCESS CODE"));Runes.ReplaceInline(TEXT(","),TEXT(""));Runes.ReplaceInline(TEXT("0"),TEXT(""));
        TestEqual(TEXT("Source runes"),FString(Spell->Runes),Runes);
        TestEqual(TEXT("Source cost"),Spell->Mana,FCString::Atoi(*Field(TEXT("SPELL COST"))));
        TestEqual(TEXT("Source hardness"),Spell->Hardness,FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
        TestEqual(TEXT("Individual tier"),Spell->Power,FCString::Atoi(*Field(TEXT("SPELL POWER"))));
        TestEqual(TEXT("Original duration"),Spell->DurationTicks,FCString::Atoi(*Field(TEXT("SPELL DURATION"))));
        TestEqual(TEXT("Source map subtype"),Field(TEXT("EFFECT SUBTYPE")),FString(TEXT("MAGIC MAP")));
        TestTrue(TEXT("Native map is an owned status"),Spell->Effect==ESpellEffect::Status&&Spell->Status==MagicMapStatus);
        const int32 Class=Spell->School==ESpellSchool::Mage?MageClass:Spell->School==ESpellSchool::Arcane?KnightClass:PriestClass;
        FMapFixture F(Class);if(!TestNotNull(TEXT("Rune player"),F.Player)){return false;}
        TestTrue(TEXT("Actual keypad and RightMouse cast"),F.Cast(Spell->Runes));
        TestEqual(TEXT("Source strict charge"),F.Player->GetMana(),30000-Spell->Mana);
        TestEqual(TEXT("Source owner duration"),F.Player->GetOwnedEffectRemaining(FWadPlayerEffects::SpellOwner(Definition)),27999.0/70.0);
        TestEqual(TEXT("Individual map tier through casting"),F.Player->GetMagicMap().Tier,Spell->Power);
        TestTrue(TEXT("Cast forces display on"),F.Player->GetMagicMap().bOn);
        TestTrue(TEXT("Source row1 hides"),F.Press(EKeys::One));TestFalse(TEXT("Hidden map"),F.Player->GetMagicMap().bOn);
        TestTrue(TEXT("Source row1 restores"),F.Press(EKeys::One));TestTrue(TEXT("Shown map"),F.Player->GetMagicMap().bOn);
        const int32 Position=F.Player->GetMagicMap().Position;F.Press(EKeys::Seven);
        TestEqual(TEXT("Row7 cycles six source positions"),F.Player->GetMagicMap().Position,(Position+1)%6);
        F.Player->Tick(400.0f);TestFalse(TEXT("Last owner expiry switches off"),F.Player->GetMagicMap().bOn);
        FMapFixture Refused(Class,Spell->Mana);Refused.Cast(Spell->Runes);
        TestEqual(TEXT("Exact cost refuses unchanged mana"),Refused.Player->GetMana(),Spell->Mana);
        TestEqual(TEXT("Refusal creates no owner"),Refused.Player->GetMagicMap().OwnerCount(),0);
    }
    FMapFixture None(MageClass);None.Press(EKeys::One);
    TestFalse(TEXT("No free map from row1"),None.Player->GetMagicMap().bOn);
    TestEqual(TEXT("Original refusal feedback"),None.Player->GetInteractionMessage(),FString(TEXT("Magic map only available with spell")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMagicMapOwners,"AAOriginal.WAD.Gameplay.MagicMapOwners",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalMagicMapOwners::RunTest(const FString& Parameters)
{
    using M=FWadMagicMap;M Map;TMap<FString,int32> Active;
    Active.Add(TEXT("spell:44"),4);Map.SynchronizeOwners(Active);
    TestEqual(TEXT("Tier4 features"),int32(Map.Features),int32(M::Rotate|M::Translucent|M::AllWalls|M::Objects|M::Creatures));
    Map.Command(2);Map.Command(3);Map.Command(4);Map.Command(1);
    Active.Add(TEXT("jewelry:map:1"),1);Map.StartOwner(TEXT("jewelry:map:1"),1);Map.SynchronizeOwners(Active);
    TestEqual(TEXT("Ring READY downgrades tier rather than sums"),Map.Tier,1);
    TestTrue(TEXT("READY forces display on"),Map.bOn);
    TestEqual(TEXT("READY resets source rotate/translucent flags"),int32(Map.Features),int32(M::Rotate|M::Translucent));
    Map.StartOwner(TEXT("spell:44"),4);TestEqual(TEXT("Same owner recast resets latest tier"),Map.Tier,4);
    Active.Remove(TEXT("spell:44"));Map.SynchronizeOwners(Active);
    TestEqual(TEXT("Removing configuring owner does not recompute remaining tier"),Map.Tier,4);
    TestTrue(TEXT("Remaining ring keeps map on"),Map.bOn);
    Active.Reset();Map.SynchronizeOwners(Active);TestFalse(TEXT("Safe final-owner-off correction"),Map.bOn);
    const auto* Catalog=LoadObject<UNativeWadHudData>(nullptr,UNativeWadHudData::ObjectPath);
    const auto* Ring=Catalog?Catalog->ItemDetails.Find(435):nullptr;
    if(!TestNotNull(TEXT("Source map ring saved in item catalog"),Ring)){return false;}
    TestEqual(TEXT("Ring source READY map status"),Ring->Status,int32(NativeWadRules::MagicMapStatus));
    TestEqual(TEXT("Ring individual power1"),Ring->StatusPower,1);
    TestEqual(TEXT("Ring until-UNREADY duration"),Ring->StatusTicks,0);
    FMapFixture F(NativeWadRules::MageClass);if(!F.Player){return false;}
    TestTrue(TEXT("Ring435 in catalog after item refresh"),F.Player->CollectItem(435));
    TestTrue(TEXT("Take map ring"),F.Player->TakeInventoryItem(F.Player->GetCarriedItems().Num()-1));
    TestEqual(TEXT("Held ring has no effect"),F.Player->GetMagicMap().OwnerCount(),0);
    F.Player->ClickJewelrySlot(NativeWadRules::Ring1Slot);
    TestEqual(TEXT("Worn source ring activates tier1"),F.Player->GetMagicMap().Tier,1);
    F.Cast(TEXT("4967"));TestEqual(TEXT("Timed tier4 overrides ring"),F.Player->GetMagicMap().Tier,4);
    F.Player->ReceiveTypedHit(1,NativeWadRules::SpecialDamageFlag(NativeWadRules::ESpecialDamage::Dispel));
    TestEqual(TEXT("Dispel reREADIES ring at tier1"),F.Player->GetMagicMap().Tier,1);
    F.Player->ClickJewelrySlot(NativeWadRules::Ring1Slot);
    TestFalse(TEXT("Last ring unREADY shuts display"),F.Player->GetMagicMap().bOn);
    M SavedMap;SavedMap.BindTopology(TEXT("L30"),2,TEXT("1234ABCD"));SavedMap.Observe(TEXT("L30"),1);
    SavedMap.StartOwner(TEXT("spell:44"),4);SavedMap.Command(2);SavedMap.Command(7);SavedMap.Command(1);
    FWadPlayerEffects Effects;Effects.Apply({TEXT("spell:44"),EWadTimedEffectKind::Status,NativeWadRules::MagicMapStatus,4,100.0});
    const FString Options=TEXT("WadClass=2?WadCreated=1?WadItems=435?WadReady=0?WadOwnedEffects=")+Effects.Encode()
        +TEXT("?WadMagicMap=")+SavedMap.Encode();
    FMapFixture Loaded(NativeWadRules::MageClass,30000,Options);
    TestEqual(TEXT("Fresh character restores timed map plus readied ring"),Loaded.Player->GetMagicMap().OwnerCount(),2);
    TestEqual(TEXT("Native restore preserves last-started individual tier"),Loaded.Player->GetMagicMap().Tier,4);
    TestEqual(TEXT("Native restore preserves player display/rotation choices"),int32(Loaded.Player->GetMagicMap().Features),int32(SavedMap.Features));
    TestFalse(TEXT("Load does not force hidden map back on"),Loaded.Player->GetMagicMap().bOn);
    TestTrue(TEXT("Saved per-map exploration restores through normal options"),Loaded.Player->GetMagicMap().IsSeen(TEXT("L30"),1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMagicMapRaster,"AAOriginal.WAD.Geometry.MagicMapRaster",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalMagicMapRaster::RunTest(const FString& Parameters)
{
    using M=FWadMagicMap;M Map;Map.BindTopology(TEXT("L30"),4,TEXT("1234ABCD"));Map.Observe(TEXT("L30"),0);
    TestTrue(TEXT("Observed line is eligible"),M::Eligible(0,true,0));
    TestTrue(TEXT("Authored0x100 is eligible without walking"),M::Eligible(0x100,false,0));
    TestFalse(TEXT("Invisible0x80 stays hidden even under all walls"),M::Eligible(0x180,true,M::AllWalls));
    TestEqual(TEXT("Impassable color31"),int32(M::LineColor(1)),31);TestEqual(TEXT("AlwaysSolid color31"),int32(M::LineColor(0x40)),31);
    TestEqual(TEXT("Other line color47"),int32(M::LineColor(0)),47);
    const FIntPoint Positions[]={{7,6},{237,6},{237,72},{7,72},{122,39},{7,72}};
    for(int32 I=0;I<6;++I){Map.Position=I;TestEqual(TEXT("Source75px opening placement"),Map.Location(FIntRect(4,3,315,150)),Positions[I]);}
    Map.Position=1;TestEqual(TEXT("Banner narrows current3D opening"),Map.Location(FIntRect(4,3,204,150)),FIntPoint(126,6));
    Map.Features=0;TestEqual(TEXT("Native east projects right with rotation disabled"),Map.Project({80,0},{0,0},0),FIntPoint(47,37));
    Map.Features=M::Rotate;TestEqual(TEXT("Rotate puts forward up without arrow"),Map.Project({80,0},{0,0},0),FIntPoint(37,27));
    TArray<M::FLine> Lines;auto L=Wall(0);L.Wall.From={-10000,0};L.Wall.To={10000,0};L.Flags=1;Lines.Add(L);
    Map.Features=0;auto Page=Map.Rasterize(TEXT("L30"),Lines,{},FVector2D::ZeroVector,0,10);
    TestEqual(TEXT("Exactly75square"),Page.Num(),75*75);TestEqual(TEXT("Clipped wall reaches left"),int32(Page[37*75]),31);
    TestEqual(TEXT("Clipped wall reaches right"),int32(Page[37*75+74]),31);TestEqual(TEXT("Player center color160"),int32(Page[37*75+37]),160);
    TArray<M::FMarker> Markers={{{160,0},16,false,false,false},{{0,160},16,true,false,true},{{-160,0},16,false,true,false}};
    Map.Features=M::Objects;Page=Map.Rasterize(TEXT("L30"),{},Markers,{},0,10);
    TestEqual(TEXT("Ordinary scenery marker"),int32(Page[37*75+57]),31);
    TestEqual(TEXT("Creature absent without creature bit"),int32(Page[17*75+37]),0);
    TestEqual(TEXT("Invisible object suppressed"),int32(Page[37*75+17]),0);
    Map.Features|=M::Creatures;Page=Map.Rasterize(TEXT("L30"),{},Markers,{},0,10);
    TestEqual(TEXT("Creature extra-data center47"),int32(Page[17*75+37]),47);
    TestEqual(TEXT("Creature source radius outline31"),int32(Page[15*75+35]),31);
    Map.StartOwner(TEXT("spell:44"),4);const FString Saved=Map.Encode();M Restored;
    TestTrue(TEXT("Character travel/checkpoint payload roundtrip"),Restored.Decode(Saved));
    Restored.BindTopology(TEXT("L30"),4,TEXT("1234ABCD"));TestTrue(TEXT("Matching topology preserves seen"),Restored.IsSeen(TEXT("L30"),0));
    Restored.BindTopology(TEXT("L301"),4,TEXT("1234ABCD"));TestFalse(TEXT("Per-map seen separation"),Restored.IsSeen(TEXT("L301"),0));
    Restored.BindTopology(TEXT("L30"),4,TEXT("DEADBEEF"));TestFalse(TEXT("Different geometry invalidates old seen"),Restored.IsSeen(TEXT("L30"),0));
    TestFalse(TEXT("Partial integer payload refused"),Restored.Decode(TEXT("1,1,79,4,1,8192~L30,4,1234ABCD,0x")));
    TestFalse(TEXT("Out of topology line refused"),Restored.Decode(TEXT("1,1,79,4,1,8192~L30,4,1234ABCD,4")));
    TestFalse(TEXT("Tier1 cannot restore invented all-wall sight"),Restored.Decode(TEXT("1,1,79,1,1,8192")));
    TestFalse(TEXT("Tier2 cannot restore invented all-wall sight"),Restored.Decode(TEXT("1,1,79,2,1,8192")));
    TestFalse(TEXT("Unused perspective mode cannot enter saved state"),Restored.Decode(TEXT("1,1,104,1,1,8192")));
    TestFalse(TEXT("A never-activated tier0 map cannot restore on"),Restored.Decode(TEXT("1,1,72,0,1,8192")));
    for(int32 Tier=1;Tier<=4;++Tier)
    {
        M Canonical;Canonical.StartOwner(TEXT("spell:44"),Tier);Canonical.Command(2);Canonical.Command(3);Canonical.Command(4);
        const FString Encoding=Canonical.Encode();M Valid;
        TestTrue(TEXT("Every original tier with all allowed user toggles restores"),Valid.Decode(Encoding));
        TestEqual(TEXT("Validated flags roundtrip"),int32(Valid.Features),int32(Canonical.Features));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMagicMapObserver,"AAOriginal.WAD.Geometry.MagicMapObserver",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalMagicMapObserver::RunTest(const FString& Parameters)
{
    using M=FWadMagicMap;TArray<M::FLine> Lines={Wall(100),Wall(50)};const FVector Eye(0,0,50);
    TestFalse(TEXT("Closed opaque door occludes farther wall"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[1].Wall.Surfaces.Reset();TestTrue(TEXT("Open door reveals textured wall"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    TestFalse(TEXT("Facing away does not reveal"),M::CanObserve(Lines,0,Eye,FRotator(0,180,0),90,1.6,4));
    TestFalse(TEXT("Textureless portal not itself seen"),M::CanObserve(Lines,1,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[1]=Wall(50,0,100,EWadWallBlend::Translucent);
    TestTrue(TEXT("Translucent band permits farther observation"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[1]=Wall(50,100,200);TestTrue(TEXT("Upper band above eye leaves opening"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[1]=Wall(50,-100,0);TestTrue(TEXT("Lower band below eye leaves opening"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[0]=Wall(100,300,400);TestFalse(TEXT("Out-of-frustum upper band not seen"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[0]=Wall(100,-300,-200);TestFalse(TEXT("Out-of-frustum lower band not seen"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4));
    Lines[0]=Wall(100);TestFalse(TEXT("Actual3D opening crop rejects hidden projected band"),M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4,
        FBox2D(FVector2D(0,0),FVector2D(0.4,1))));
    M Hidden;Hidden.BindTopology(TEXT("L30"),2,TEXT("1234ABCD"));
    if(M::CanObserve(Lines,0,Eye,FRotator::ZeroRotator,90,1.6,4)){Hidden.Observe(TEXT("L30"),0);}
    TestFalse(TEXT("Observer does not activate display"),Hidden.bOn);TestTrue(TEXT("Observation retained while map hidden"),Hidden.IsSeen(TEXT("L30"),0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMagicMapSourceMarkers,"AAOriginal.WAD.Gameplay.MagicMapSourceMarkers",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalMagicMapSourceMarkers::RunTest(const FString& Parameters)
{
    TArray<FString> Source;
    if(!TestTrue(TEXT("Read original player type510 definition"),FFileHelper::LoadFileToStringArray(Source,*(FPaths::ProjectDir()/
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/OBJS/HEAD002/HEAD002.INF"))))){return false;}
    TArray<int32> Values;
    for(FString Row:Source)
    {
        FString Value,Comment;if(Row.Split(TEXT(";"),&Value,&Comment)){Row=Value;}Row.TrimStartAndEndInline();
        if(!Row.IsEmpty()){Values.Add(FCString::Atoi(*Row));}
    }
    if(!TestTrue(TEXT("Original header contains radius"),Values.Num()>1)){return false;}
    TestEqual(TEXT("Map player radius comes from original object, not native capsule"),FWadMagicMap::PlayerRadius,Values[1]);
    FWadMagicMap Map;const auto Page=Map.Rasterize(TEXT("L30"),{},{},{},0,FWadMagicMap::PlayerRadius);
    TestEqual(TEXT("Original player square extends two pixels at default zoom"),int32(Page[35*75+35]),31);
    FMapFixture Scene(NativeWadRules::MageClass,30000,FString(),true);
    if(!TestNotNull(TEXT("Native marker level"),Scene.Level)||!Scene.Player){return false;}
    auto* Scenery=Scene.World->SpawnActor<ANativeWadScenery>();auto* Pickup=Scene.World->SpawnActor<ANativeWadPickup>();
    if(!Scenery||!Pickup){return false;}Scenery->SetPickup(Pickup);
    TestEqual(TEXT("Visible scenery/item pair is one marker"),Scene.Player->GetMagicMapMarkers().Num(),1);
    Scenery->SetActorHiddenInGame(true);
    TestEqual(TEXT("Hidden scenery cannot leak its visible pickup proxy"),Scene.Player->GetMagicMapMarkers().Num(),0);
    Scenery->SetActorHiddenInGame(false);Pickup->SetAvailable(false);
    TestEqual(TEXT("Unavailable pickup cannot leak its still-visible scenery"),Scene.Player->GetMagicMapMarkers().Num(),0);
    Pickup->SetAvailable(true);Pickup->SetAccessoryData(6);
    TestEqual(TEXT("Auxiliary payload6 is not invisible object attributes"),Scene.Player->GetMagicMapMarkers().Num(),1);
    FWadSceneryDefinition Definition;Definition.SourceAttributes=0x0004;Definition.Radius=20;Definition.Frames.AddDefaulted();
    Definition.Frames[0].Views.AddDefaulted();Definition.Frames[0].Views[0].Size=FIntPoint(8,8);
    Scenery->Configure(*Scene.Level,nullptr,Definition,INDEX_NONE,0);
    Scenery->SetActorLocation(FVector(160,0,0));
    auto Markers=Scene.Player->GetMagicMapMarkers();
    if(!TestEqual(TEXT("Invisible object remains one attributed source record"),Markers.Num(),1)){return false;}
    TestTrue(TEXT("Original invisible0x4 reaches raster input"),Markers[0].bInvisible);
    Map.Features=FWadMagicMap::Objects;
    const auto EmptyPage=Map.Rasterize(TEXT("L30"),{},{},{},0,FWadMagicMap::PlayerRadius);
    TestTrue(TEXT("Original invisible0x4 produces no object pixels"),
        Map.Rasterize(TEXT("L30"),{},Markers,{},0,FWadMagicMap::PlayerRadius)==EmptyPage);
    Markers[0].bInvisible=false;
    TestTrue(TEXT("Same visible object produces actual map pixels away from player"),
        Map.Rasterize(TEXT("L30"),{},Markers,{},0,FWadMagicMap::PlayerRadius)!=EmptyPage);
    return true;
}
