#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DoomWadLoader.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    struct FAcidFixture
    {
        UGameInstance* Instance=nullptr;
        UWorld* World=nullptr;
        ANativeWadLevel* Level=nullptr;
        ANativeWadPlayer* Player=nullptr;
        UStaticMeshComponent* VisibleFloor=nullptr;
        UStaticMeshComponent* WalkingFloor=nullptr;
        explicit FAcidFixture(const TCHAR* OwnedEffects=nullptr,const TCHAR* SavedOptions=nullptr,bool bWarmup=true,int32 InitialHealth=30000)
        {
            Instance=NewObject<UGameInstance>(GEngine);Instance->InitializeStandalone();World=Instance->GetWorld();
            if (!World) { return; }
            const FString Options=SavedOptions?FString(SavedOptions):FString(TEXT("/Game/Maps/L75?WadClass=1?WadCreated=1?WadRuneRules=1")
                TEXT("?WadLevel=100?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=30000")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadItems=?WadReady=-1"));
            World->URL=FURL(nullptr,*Options,TRAVEL_Absolute);
            if (!SavedOptions) { World->URL.AddOption(*FString::Printf(TEXT("WadHealth=%d"),InitialHealth)); }
            if (OwnedEffects) { World->URL.AddOption(*(TEXT("WadOwnedEffects=")+FString(OwnedEffects))); }
            Level=World->SpawnActor<ANativeWadLevel>();
            const auto* Property=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
            if (!Level||!Property) { return; }
            auto& Sectors=*Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
            auto& Sector=Sectors.AddDefaulted_GetRef();
            Sector.bAcid=true;Sector.FloorDepth=30;Sector.SourceFriction=24;
            Sector.RepairedBoundaryPoints={{-1000,-1000},{1000,-1000},{1000,-1000},{1000,1000},
                {1000,1000},{-1000,1000},{-1000,1000},{-1000,-1000}};
            const auto Surface=[&](const TCHAR* Name,double Z)
            {
                auto* Result=NewObject<UStaticMeshComponent>(Level,Name);
                Result->SetupAttachment(Level->GetRootComponent());Result->SetMobility(EComponentMobility::Movable);
                Result->SetRelativeLocation(FVector(0,0,Z));Result->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                Result->RegisterComponent();return Result;
            };
            VisibleFloor=Sector.Floor=Surface(TEXT("Sector_0000_Floor"),-20);
            Sector.Ceiling=Surface(TEXT("Sector_0000_Ceiling"),2000);
            WalkingFloor=Sector.WalkingFloor=Surface(TEXT("WalkingFloor_0000"),-50);
            WalkingFloor->SetVisibility(false);
            auto* Collision=NewObject<UBoxComponent>(Level);
            Collision->SetupAttachment(WalkingFloor);Collision->SetMobility(EComponentMobility::Movable);
            Collision->SetBoxExtent(FVector(1000,1000,2));Collision->SetRelativeLocation(FVector(0,0,-2));
            Collision->SetCollisionProfileName(TEXT("BlockAll"));Collision->SetCanEverAffectNavigation(false);Collision->RegisterComponent();
            Level->SetSectorFloorZ(0,-20);
            Player=World->SpawnActor<ANativeWadPlayer>(FVector(0,0,-15+ANativeWadPlayer::CapsuleHalfHeight),FRotator::ZeroRotator);
            auto* Controller=World->SpawnActor<APlayerController>();
            if (!Player||!Controller) { return; }
            World->AddController(Controller);Controller->Possess(Player);Player->DispatchBeginPlay();
            if (bWarmup) { Frames(120); }
        }
        ~FAcidFixture()
        {
            if (Instance) { Instance->Shutdown(); }
            if (World) { GEngine->DestroyWorldContext(World);World->DestroyWorld(false); }
        }
        bool Valid()const {return Player&&Level&&WalkingFloor;}
        void Frames(int32 Count)const
        {
            for(int32 I=0;I<Count;++I)
            {Player->Tick(1.0f/240.0f);Player->GetCharacterMovement()->TickComponent(1.0f/240.0f,LEVELTICK_All,nullptr);}
        }
        double Feet()const{return Player->GetActorLocation().Z-Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();}
        void SetFeet(double Z)const
        {
            Player->GetCharacterMovement()->StopMovementImmediately();
            Player->SetActorLocation(FVector(0,0,Z+Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()),false,nullptr,ETeleportType::TeleportPhysics);
        }
        int32 Pulse()const
        {
            const int32 Before=Player->GetHealth();Player->Tick(0.51f);return Before-Player->GetHealth();
        }
        bool Wear(int32 Type,int32 Location,bool bArmor=false)const
        {
            if(!Player->CollectItem(Type)||!Player->TakeInventoryItem(Player->GetCarriedItems().Num()-1)){return false;}
            if(bArmor){Player->ClickArmorSlot(Location);}else{Player->ClickJewelrySlot(Location);}
            return Player->HasEquippedItem(Type)||Player->IsWornInventorySlot(Player->GetCarriedItems().Num()-1);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAcidTerrainSource,"AAOriginal.WAD.Gameplay.AcidTerrainSource",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAcidTerrainSource::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const FString Root=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor");
    FString Source;
    if(!FFileHelper::LoadFileToString(Source,*(Root/TEXT("Source/3D_IO.C")))){return false;}
    const int32 Start=Source.Find(TEXT("/* Type 32:  SECTOR_TYPE_ACID */"));
    if(!TestTrue(TEXT("Read the exact original acid physical-sector template"),Start!=INDEX_NONE)){return false;}
    const int32 End=Source.Find(TEXT("},"),ESearchCase::CaseSensitive,ESearchDir::FromStart,Start);
    TArray<FString> Raw,Fields;Source.Mid(Start,End-Start).ParseIntoArrayLines(Raw);
    for(const FString& Line:Raw)
    {
        const int32 Comment=Line.Find(TEXT("/*"));
        FString Value=(Comment==INDEX_NONE?Line:Line.Left(Comment)).TrimStartAndEnd();
        if(Value.RemoveFromEnd(TEXT(","))&&!Value.IsEmpty()){Fields.Add(Value.TrimStartAndEnd());}
    }
    if(!TestTrue(TEXT("Original physical acid template has its complete fields"),Fields.Num()>=16)){return false;}
    TestEqual(TEXT("Original acid raw damage"),SourceAcidDamage,FCString::Atoi(*Fields[5]));
    TestEqual(TEXT("Original damage combines normal and acid"),Fields[6],FString(TEXT("EFFECT_DAMAGE_NORMAL | EFFECT_DAMAGE_ACID")));
    TestEqual(TEXT("Original sink depth"),SourceAcidDepth,FCString::Atoi(*Fields[7]));
    TestEqual(TEXT("Acid shares the source friction24 value without becoming lava"),FCString::Atoi(*Fields[4]),24);
    TestEqual(TEXT("Original half-second cadence is35 source ticks"),SourceSectorDamageTicks,35);
    if(!UDoomWadLoader::LoadDoomWadMap(Root/TEXT("Exe/L75.MAP"),TEXT("L75"))){return false;}
    const auto* Map=UDoomWadLoader::GetLastWadMapData();
    if(!Map||!Map->Things.IsValidIndex(281)||!Map->Sectors.IsValidIndex(166)){return false;}
    TestEqual(TEXT("Exact original acid marker identity"),int32(Map->Things[281].Type),1543);
    TestEqual(TEXT("Original acid floor height"),int32(Map->Sectors[166].FloorHeight),-20);
    TestEqual(TEXT("Legacy fire-damage flag does not override this acid sector"),Map->Sectors[166].Trigger&2,0);
    FAcidFixture Fixture;
    if(!Fixture.Valid()){return false;}
    TestTrue(TEXT("Native physical acid metadata is distinct"),Fixture.Level->IsWadSectorAcid(0));
    TestEqual(TEXT("Native saved acid depth matches original"),Fixture.Level->GetWadSectorDepth(0),30);
    TestEqual(TEXT("Native lowered walking floor is original minus depth"),Fixture.Level->GetWalkingHeight(0),-50.0);
    TestFalse(TEXT("Water surface lookup cannot mistake acid for water"),Fixture.Level->FindWaterSurfaceZ(FVector::ZeroVector).IsSet());
    TestFalse(TEXT("Lava surface lookup cannot mistake acid for lava"),Fixture.Level->FindLavaSurfaceZ(FVector::ZeroVector).IsSet());
    Fixture.Level->SetActorLocation(FVector(1000,2000,300));
    TestEqual(TEXT("Translated level queries apply the transform exactly once"),Fixture.Level->FindAcidSurfaceZ(FVector(1000,2000,350)).Get(0),280.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAcidStandingAndWalking,"AAOriginal.WAD.Gameplay.AcidStandingAndWalking",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAcidStandingAndWalking::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FAcidFixture Ordinary;
    if(!Ordinary.Valid()){return false;}
    TestTrue(TEXT("Actual character physics settles on the dipped acid floor"),Ordinary.Player->GetCharacterMovement()->IsMovingOnGround()
        &&FMath::Abs(Ordinary.Feet()+50)<3);
    TestEqual(TEXT("Actual exposed standing player takes typed200 damage per pulse"),Ordinary.Pulse(),200);
    Ordinary.SetFeet(-10);
    TestEqual(TEXT("A player clearly above the visible surface takes no terrain damage"),Ordinary.Pulse(),0);
    Ordinary.SetFeet(-19.5);
    TestEqual(TEXT("Source signed-integer feet contact includes the fractional unit above the floor"),Ordinary.Pulse(),200);
    Ordinary.SetFeet(-50);
    const int32 Before=Ordinary.Player->GetHealth();Ordinary.Player->Tick(2.0f);
    TestEqual(TEXT("A long frame applies one due pulse instead of fabricating a backlog"),Before-Ordinary.Player->GetHealth(),200);
    const FString Water=FString::Printf(TEXT("1~spell:71,%d,-1,1,60"),int32(EWadTimedEffectKind::WaterWalk));
    const FString Lava=FString::Printf(TEXT("1~spell:92,%d,%d,1,60"),int32(EWadTimedEffectKind::Status),int32(LavaWalkStatus));
    for(const FString& Effect:{Water,Lava})
    {
        FAcidFixture Walker(*Effect);
        if(!Walker.Valid()){return false;}
        TestTrue(TEXT("Water/Lava Walk still physically sinks to the acid bottom"),Walker.Player->GetCharacterMovement()->IsMovingOnGround()
            &&FMath::Abs(Walker.Feet()+50)<3);
        TestEqual(TEXT("Neither walking effect grants an invented acid exemption"),Walker.Pulse(),200);
    }
    const FString Flight=FString::Printf(TEXT("1~spell:41,%d,-1,1,60"),int32(EWadTimedEffectKind::Flight));
    FAcidFixture Flying(*Flight,nullptr,false);
    if(!Flying.Valid()){return false;}
    Flying.SetFeet(100);
    TestEqual(TEXT("Flight above acid avoids contact damage"),Flying.Pulse(),0);
    TestTrue(TEXT("The actual restored effect selects flying movement"),Flying.Player->GetCharacterMovement()->IsFlying());
    Flying.SetFeet(-21);
    TestEqual(TEXT("Flying does not invent immunity when feet enter acid"),Flying.Pulse(),200);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAcidProtection,"AAOriginal.WAD.Gameplay.AcidProtection",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAcidProtection::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FAcidFixture Gear;
    if(!Gear.Valid()||!Gear.Wear(616,NeckSlot)){return false;}
    TestEqual(TEXT("Source amulet supplies acid resistance"),Gear.Player->GetStatusPower(AcidResistStatus),1);
    TestEqual(TEXT("Mixed Normal|Acid retains half damage with acid resistance"),Gear.Pulse(),100);
    TestTrue(TEXT("Resistance does not move the player onto the surface"),FMath::Abs(Gear.Feet()+50)<3);
    if(!Gear.Wear(45765,ChestSlot,true)){return false;}
    TestEqual(TEXT("Actual Adaminium plate has source weighted armor22"),Gear.Player->GetArmorRating(),22);
    TestEqual(TEXT("Two acid resistance owners still give one resistant damage bit"),Gear.Player->GetStatusPower(AcidResistStatus),2);
    TestEqual(TEXT("Resistance halves first, then source weighted armor mitigates the remaining hit"),Gear.Pulse(),89);
    Gear.Player->ClickArmorSlot(ChestSlot);Gear.Player->StoreMouseHandInInventory();
    TestEqual(TEXT("Removing plate retains the independent amulet"),Gear.Pulse(),100);
    Gear.Player->ClickJewelrySlot(NeckSlot);Gear.Player->StoreMouseHandInInventory();
    TestEqual(TEXT("Final equipped owner removal restores full acid damage"),Gear.Pulse(),200);
    const FString Immune=FString::Printf(TEXT("1~spell:42,%d,%d,1,60"),int32(EWadTimedEffectKind::Status),int32(InvulnerableStatus));
    FAcidFixture Invulnerable(*Immune);
    if(!Invulnerable.Valid()){return false;}
    TestEqual(TEXT("Acid uses the ordinary typed Invulnerability gate"),Invulnerable.Pulse(),0);
    const int32 Health=Invulnerable.Player->GetHealth();Invulnerable.Player->Tick(60.1f);
    TestEqual(TEXT("Real owner expiry resumes acid damage"),Health-Invulnerable.Player->GetHealth(),200);
    const FString Ward=FString::Printf(TEXT("1~spell:12,%d,%d,1,60"),int32(EWadTimedEffectKind::Status),int32(DeathWardStatus));
    FAcidFixture Warded(*Ward,nullptr,true,100);
    if(!Warded.Valid()){return false;}
    const int32 Mana=Warded.Player->GetMana();Warded.Pulse();
    TestTrue(TEXT("A source acid overkill leaves Ward alive at zero health"),Warded.Player->GetHealth()==0&&!Warded.Player->IsDefeated());
    TestEqual(TEXT("The same typed hit charges Ward the full200 health damage"),Warded.Player->GetMana(),Mana-200);
    Warded.Player->Tick(60.1f);
    TestTrue(TEXT("An acid hit after Ward expiry defeats the zero-health player"),Warded.Player->IsDefeated());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAcidPersistence,"AAOriginal.WAD.Gameplay.AcidPersistence",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalAcidPersistence::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FAcidFixture First;
    if(!First.Valid()||!First.Wear(616,NeckSlot)){return false;}
    FWadLevelCheckpoint Visit;
    First.Level->SetSectorFloorZ(0,-18);
    if(!First.Level->WriteCheckpoint(Visit)){return false;}
    First.Level->SetSectorFloorZ(0,-20);
    if(!First.Level->RestoreCheckpoint(Visit)){return false;}
    TestEqual(TEXT("Revisiting an altered floor retains original acid depth"),First.Level->GetWadSectorDepth(0),30);
    TestEqual(TEXT("The hidden walking floor follows restored visible floor minus30"),First.WalkingFloor->GetRelativeLocation().Z,-48.0);
    TestTrue(TEXT("Revisiting preserves physical acid identity"),First.Level->IsWadSectorAcid(0));
    const FURL TownURL(nullptr,TEXT("/Game/Maps/Town?game=/Script/AAOriginal.NativeWadTownGameMode"),TRAVEL_Absolute);
    if(!First.World->SetGameMode(TownURL)){return false;}
    const FString Slot=TEXT("NativeAcid-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT{UGameplayStatics::DeleteGameInSlot(Slot,0);};
    if(!TestTrue(TEXT("Save the real acid-exposed character with its worn protection"),First.Player->SaveWadCheckpoint(Slot))){return false;}
    auto* Raw=UGameplayStatics::LoadGameFromSlot(Slot,0);
    if(!Raw){return false;}
    const auto* Saved=static_cast<UNativeWadCheckpoint*>(Raw);
    FAcidFixture Reopened(nullptr,*(Saved->LevelPackage+TEXT("?")+Saved->PlayerOptions),false);
    if(!Reopened.Valid()){return false;}
    TestEqual(TEXT("A new character restores acid-protection ownership"),Reopened.Player->GetStatusPower(AcidResistStatus),1);
    Reopened.Frames(120);
    TestTrue(TEXT("The reopened character physically settles on the authored dipped floor"),FMath::Abs(Reopened.Feet()+50)<3);
    TestEqual(TEXT("Actual hazard damage remains resistant after save/reopen"),Reopened.Pulse(),100);
    return true;
}
