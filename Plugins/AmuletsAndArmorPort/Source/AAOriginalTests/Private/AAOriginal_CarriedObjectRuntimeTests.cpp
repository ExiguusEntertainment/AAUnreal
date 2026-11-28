#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCarriedObjectState.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadTown.h"

namespace AAOriginalCarriedRuntimeTests
{
    struct FCarriedDataFixture
    {
        UGameInstance* Instance=nullptr;
        UWorld* World=nullptr;
        ANativeWadPlayer* Player=nullptr;
        APlayerController* Controller=nullptr;
        UInputComponent* Input=nullptr;
        explicit FCarriedDataFixture(const TCHAR* Options=nullptr,bool bFloor=false,bool bTown=false)
        {
            Instance=NewObject<UGameInstance>(GEngine);Instance->InitializeStandalone();World=Instance->GetWorld();
            if(!World){return;}
            World->URL=FURL(nullptr,Options?Options:TEXT("/Game/Maps/L30?WadClass=4?WadCreated=1?WadRuneRules=1?WadReady=-1?WadItems=?WadMaxHealth=30000?WadHealth=10000?WadDefeated=0"),TRAVEL_Absolute);
            if(bTown)
            {
                const FURL TownURL(nullptr,TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"),TRAVEL_Absolute);
                if(!World->SetGameMode(TownURL)){return;}
            }
            if(bFloor)
            {
                auto* Level=World->SpawnActor<ANativeWadLevel>();
                const auto* Property=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
                if(!Level||!Property){return;}
                auto& Sectors=*Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level);
                auto& Sector=Sectors.AddDefaulted_GetRef();
                Sector.RepairedBoundaryPoints={{-500,-500},{500,-500},{500,-500},{500,500},{500,500},{-500,500},{-500,500},{-500,-500}};
                Sector.Floor=NewObject<UStaticMeshComponent>(Level);Sector.Ceiling=NewObject<UStaticMeshComponent>(Level);
                Sector.Floor->SetupAttachment(Level->GetRootComponent());Sector.Ceiling->SetupAttachment(Level->GetRootComponent());
                Sector.Ceiling->SetRelativeLocation(FVector(0,0,256));Sector.Floor->RegisterComponent();Sector.Ceiling->RegisterComponent();Level->SetSectorFloorZ(0,0);
            }
            Controller=World->SpawnActor<APlayerController>();
            if(!Controller){return;}World->AddController(Controller);
            SpawnPlayer();
        }
        ~FCarriedDataFixture()
        {if(Instance){Instance->Shutdown();}if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}}
        void SpawnPlayer()
        {
            Player=World->SpawnActor<ANativeWadPlayer>(FVector(100,0,32),FRotator::ZeroRotator);
            if(!Player){return;}Controller->Possess(Player);Player->DispatchBeginPlay();
            Input=NewObject<UInputComponent>(Player);Player->SetupPlayerInputComponent(Input);
        }
        bool Valid() const {return Player&&Input;}
        UNativeWadTravelState* Travel() const
        {
            UClass* Type=FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadTravelState"));
            return Type?static_cast<UNativeWadTravelState*>(Instance->GetSubsystemBase(Type)):nullptr;
        }
        bool Press(FKey Key) const
        {
            for(const auto& Binding:Input->KeyBindings)
            {if(Binding.KeyEvent==IE_Pressed&&Binding.Chord==FInputChord(Key)){Binding.KeyDelegate.Execute(Key);return true;}}
            return false;
        }
        bool Data(int32 Slot,int32 Type,bool bRecorded,int32 Value) const
        {
            const auto* Item=Player->GetCarriedObjectData(Slot);
            return Item&&Item->Type==Type&&Item->bAccessoryDataRecorded==bRecorded&&Item->AccessoryData==Value;
        }
        TArray<ANativeWadPickup*> Drops(int32 Type) const
        {
            TArray<ANativeWadPickup*> Result;
            for(TActorIterator<ANativeWadPickup> It(World);It;++It)
            {if(It->IsDropped()&&!It->IsActorBeingDestroyed()&&It->GetItemType()==Type){Result.Add(*It);}}
            return Result;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedMetadataRuntime,"AAOriginal.WAD.Inventory.CarriedMetadataRuntime",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedMetadataRuntime::RunTest(const FString& Parameters)
{
    using namespace AAOriginalCarriedRuntimeTests;
    FCarriedDataFixture F;if(!F.Valid()){return false;}auto* P=F.Player;
    // Explicit fixture pages use the new foundation API; Journal withdrawal UI
    // is intentionally a later milestone and is not claimed by this test.
    for(int32 Page:{407,672,0})
    {
        if(!TestTrue(TEXT("Create one physical373 directly in the retained hand without world GET"),P->CreateWadItemInMouseHandWithAccessoryData(373,Page))){return false;}
        const int32 Slot=P->GetMouseHandSlot();TestTrue(TEXT("Each physical page owns its exact recorded payload"),F.Data(Slot,373,true,Page));
        P->StoreMouseHandInInventory();
    }
    TestEqual(TEXT("Three same-type pages retain the source stack limit"),P->GetBagLayout().StackCount(P->GetBagLayout().LeaderOf(0)),3);
    TestTrue(TEXT("Type-only visual stacking does not overwrite native member payloads"),F.Data(0,373,true,407)&&F.Data(1,373,true,672)&&F.Data(2,373,true,0));
    if(!P->CreateWadItemInMouseHandWithAccessoryData(825,65000)||!P->SwapMouseAndReady()){return false;}
    TestTrue(TEXT("A real ready-hand swap preserves arbitrary source payload"),F.Data(P->GetReadySlot(),825,true,65000));
    if(!P->TakeInventoryItem(1)){return false;}
    TestTrue(TEXT("Selecting the individual page keeps its own payload while held"),F.Data(P->GetMouseHandSlot(),373,true,672));
    F.Press(EKeys::I);F.Press(EKeys::I);
    TestTrue(TEXT("Closing and reopening the original bag preserves held payload"),F.Data(P->GetMouseHandSlot(),373,true,672));
    if(!TestTrue(TEXT("Central removal of an earlier physical member succeeds"),P->ConsumeItem(373))){return false;}
    TestTrue(TEXT("Held index remapping retains the remaining exact page"),F.Data(P->GetMouseHandSlot(),373,true,672));
    TestTrue(TEXT("Ready index remapping retains its separate metadata"),F.Data(P->GetReadySlot(),825,true,65000));
    P->StoreMouseHandInInventory();
    if(!P->CollectItemWithAccessoryData(37569,65535)){return false;}
    TestTrue(TEXT("Packed full identity never becomes the shared art base"),F.Data(P->GetCarriedItems().Num()-1,37569,true,65535));
    const int32 Count=P->GetCarriedItems().Num();
    TestFalse(TEXT("An out-of-range payload cannot append a narrowed object"),P->CollectItemWithAccessoryData(373,65536));
    TestEqual(TEXT("Refused append preserves physical count"),P->GetCarriedItems().Num(),Count);
    TestTrue(TEXT("All surviving arrays remain aligned after transfers/removal"),P->HasValidCarriedObjectData());
    FCarriedDataFixture Capacity;if(!Capacity.Valid()){return false;}
    for(int32 I=0;I<5;++I){if(!Capacity.Player->CollectItemWithAccessoryData(712,I)){return false;}}
    TestFalse(TEXT("A sixth real64x64 shield cannot bypass the five-bag capacity through metadata append"),Capacity.Player->CollectItemWithAccessoryData(712,42));
    TestEqual(TEXT("Failed spatial placement rolls back physical and metadata ownership together"),Capacity.Player->GetCarriedItems().Num(),5);
    for(int32 I=0;I<5;++I){TestTrue(TEXT("A refused sixth item preserves every existing exact payload"),Capacity.Data(I,712,true,I));}
    TestTrue(TEXT("Failed capacity leaves valid metadata"),Capacity.Player->HasValidCarriedObjectData());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedMetadataWorld,"AAOriginal.WAD.Inventory.CarriedMetadataWorld",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedMetadataWorld::RunTest(const FString& Parameters)
{
    using namespace AAOriginalCarriedRuntimeTests;
    FCarriedDataFixture F(nullptr,true);if(!F.Valid()){return false;}auto* P=F.Player;
    if(!P->CreateWadItemInMouseHandWithAccessoryData(825,54321)){return false;}
    P->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    if(!TestTrue(TEXT("Normal G input drops the retained owner"),F.Press(EKeys::G))){return false;}
    auto Drops=F.Drops(825);
    if(!TestEqual(TEXT("Normal drop creates one physical object"),Drops.Num(),1)){return false;}
    TestTrue(TEXT("Normal drop copies recorded full uint16 metadata"),Drops[0]->HasAccessoryData()&&Drops[0]->GetAccessoryData()==54321);
    TestEqual(TEXT("Successful drop removes its carried owner"),P->GetItemCount(825),0);
    if(!TestTrue(TEXT("Ordinary world GET returns the same payload to a carried object"),Drops[0]->Collect(*P))){return false;}
    TestTrue(TEXT("World-to-bag collection preserves metadata"),F.Data(0,825,true,54321));
    if(!P->CreateWadItemInMouseHandWithAccessoryData(373,407)){return false;}
    F.Press(EKeys::G);Drops=F.Drops(373);
    if(Drops.Num()!=1||!TestTrue(TEXT("A dropped physical journal page keeps its identity until source GET"),Drops[0]->HasAccessoryData()&&Drops[0]->GetAccessoryData()==407)){return false;}
    if(!TestTrue(TEXT("Actual journal GET reinserts and consumes the dropped physical page"),Drops[0]->Collect(*P))){return false;}
    const auto* Travel=F.Travel();
    TestTrue(TEXT("GET restores exact page407 and leaves no373 bag object"),Travel&&Travel->JournalPages.Contains(407)&&P->GetItemCount(373)==0);
    int32 Ammo=0;
    for(int32 Case=0;Case<3;++Case)
    {
        const bool bRecorded=Case<2;const int32 Value=Case==0?6:0;
        if(!(bRecorded?P->CreateWadItemInMouseHandWithAccessoryData(147,Value):P->CreateWadItemInMouseHand(147))){return false;}
        F.Press(EKeys::G);Drops=F.Drops(147);
        if(!TestEqual(TEXT("Each retained quiver drops exactly once"),Drops.Num(),1)){return false;}
        const int32 Quantity=Case==0?6:12;
        TestTrue(TEXT("Carried quiver drop keeps recorded presence independently of zero"),Drops[0]->HasAccessoryData()==bRecorded&&Drops[0]->GetAccessoryData()==Value);
        TestEqual(TEXT("Source partial quantity and both zero/default cases resolve correctly"),Drops[0]->GetAmmoQuantity(),Quantity);
        if(!Drops[0]->Collect(*P)){return false;}Ammo+=Quantity;
        TestTrue(TEXT("Source quiver GET consumes the object and transfers the exact quantity"),P->GetItemCount(147)==0&&P->GetAmmoCount(2)==Ammo);
    }
    FCarriedDataFixture Death(nullptr,true);if(!Death.Valid()){return false;}
    if(!Death.Player->CreateWadItemInMouseHandWithAccessoryData(373,0)){return false;}
    Death.Player->StoreMouseHandInInventory();
    if(!Death.Player->CreateWadItemInMouseHandWithAccessoryData(373,672)){return false;}
    Death.Player->ReceiveEncounterHit(MAX_int32);Drops=Death.Drops(373);
    TestTrue(TEXT("Actual defeat removes both carried physical pages exactly once"),Death.Player->IsDefeated()&&Death.Player->GetItemCount(373)==0&&Drops.Num()==2);
    TSet<int32> Pages;for(const auto* Drop:Drops){TestTrue(TEXT("Each death drop retains explicit metadata"),Drop->HasAccessoryData());Pages.Add(Drop->GetAccessoryData());}
    TestTrue(TEXT("Same-type death drops keep distinct exact pages including recorded0"),Pages.Num()==2&&Pages.Contains(0)&&Pages.Contains(672));
    Death.Player->ReceiveEncounterHit(1);TestEqual(TEXT("A repeated hit cannot duplicate physical death drops"),Death.Drops(373).Num(),2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedMetadataPersistence,"AAOriginal.WAD.Inventory.CarriedMetadataPersistence",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedMetadataPersistence::RunTest(const FString& Parameters)
{
    using namespace AAOriginalCarriedRuntimeTests;
    FCarriedDataFixture F(nullptr,false,true);if(!F.Valid()){return false;}auto* P=F.Player;
    if(!P->CreateWadItemInMouseHandWithAccessoryData(373,407)){return false;}P->StoreMouseHandInInventory();
    if(!P->CreateWadItemInMouseHandWithAccessoryData(373,0)||!P->SwapMouseAndReady()){return false;}
    if(!P->CreateWadItemInMouseHandWithAccessoryData(37569,65535)){return false;}
    const FString Slot=TEXT("CarriedMetadata-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT{UGameplayStatics::DeleteGameInSlot(Slot,0);};
    if(!TestTrue(TEXT("The production checkpoint writes exact carried metadata"),P->SaveWadCheckpoint(Slot))){return false;}
    auto* Save=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    if(!TestNotNull(TEXT("Read the actual saved checkpoint"),Save)){return false;}
    TestEqual(TEXT("Per-object persistence uses version4 to prevent downgrade data loss"),Save->Version,4);
    const FString Original=Save->PlayerOptions;
    const FURL URL(nullptr,*(Save->LevelPackage+TEXT("?")+Original),TRAVEL_Absolute);
    TestTrue(TEXT("Production shared metadata preflight accepts this travel/checkpoint payload"),NativeWadCarriedObjectState::ValidateOptions(URL));
    // The same authoritative options are used for map travel and disk restore.
    // This is a fresh actor/world test, not a separate-process rendered review.
    const FString Next=TEXT("/Game/Maps/L301?")+Original;
    FCarriedDataFixture Travel(*Next);if(!Travel.Valid()){return false;}
    TestTrue(TEXT("Fresh L301 actor keeps bag, Ready and mouse-hand payloads"),Travel.Data(0,373,true,407)
        &&Travel.Data(Travel.Player->GetReadySlot(),373,true,0)&&Travel.Data(Travel.Player->GetMouseHandSlot(),37569,true,65535));
    const FString SavedURL=Save->LevelPackage+TEXT("?")+Original;
    FCarriedDataFixture Reload(*SavedURL,false,true);if(!Reload.Valid()){return false;}
    TestTrue(TEXT("Actual disk data restores the distinct same-type entries and full packed identity"),Reload.Data(0,373,true,407)
        &&Reload.Data(Reload.Player->GetReadySlot(),373,true,0)&&Reload.Data(Reload.Player->GetMouseHandSlot(),37569,true,65535));
    // Deliberately corrupt the readonly witness through a controlled test cast,
    // then prove save refusal leaves the previous valid disk record untouched.
    auto* Live=const_cast<FWadCarriedObjectData*>(P->GetCarriedObjectData(0));
    if(!Live){return false;}Live->Type=999;
    TestFalse(TEXT("Unencodable current metadata cannot overwrite a valid slot"),P->SaveWadCheckpoint(Slot));
    const auto* Unchanged=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    TestTrue(TEXT("Failed save preserves the previous exact disk payload"),Unchanged&&Unchanged->PlayerOptions==Original);
    Live->Type=373;
    TestTrue(TEXT("Restoring the controlled witness restores valid state"),P->HasValidCarriedObjectData());
    FURL Legacy(nullptr,*SavedURL,TRAVEL_Absolute);Legacy.RemoveOption(TEXT("WadObjectData"));
    Save->Version=3;Save->PlayerOptions=Legacy.ToString().Mid(Legacy.ToString().Find(TEXT("?"))+1);
    if(!UGameplayStatics::SaveGameToSlot(Save,Slot,0)){return false;}
    const FString OldURL=Save->LevelPackage+TEXT("?")+Save->PlayerOptions;
    FCarriedDataFixture Old(*OldURL,false,true);if(!Old.Valid()){return false;}
    TestTrue(TEXT("Version1-3 absence migrates ownership without guessing physical-page IDs"),Old.Data(0,373,false,0)
        &&Old.Data(Old.Player->GetReadySlot(),373,false,0)&&Old.Data(Old.Player->GetMouseHandSlot(),37569,false,0));
    P->Destroy();
    const FString LegacyLoad=FString::Printf(TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode?WadClass=4?WadCreated=1?WadRuneRules=1?WadItems=201?WadReady=0?WadHealth=1234?WadLoad=%s"),*Slot);
    F.World->URL=FURL(nullptr,*LegacyLoad,TRAVEL_Absolute);F.SpawnPlayer();F.Player->Tick(0.0f);
    TestTrue(TEXT("The production checkpoint reader actually accepts version3 absence"),F.Player->HasRestoredWadCheckpoint());
    TestTrue(TEXT("Actual legacy load retains every exact type with unrecorded defaults"),F.Data(0,373,false,0)
        &&F.Data(F.Player->GetReadySlot(),373,false,0)&&F.Data(F.Player->GetMouseHandSlot(),37569,false,0));
    TestEqual(TEXT("The deserialization class default stays stable for untagged legacy files"),
        static_cast<const UNativeWadCheckpoint*>(Save->GetClass()->GetDefaultObject())->Version,1);
    Save->Version=1;
    TArray<uint8> Untagged;
    if(!UGameplayStatics::SaveGameToMemory(Save,Untagged)){return false;}
    const ANSICHAR VersionTag[]="Version";const int32 VersionTagSize=UE_ARRAY_COUNT(VersionTag);bool bTagged=false;
    for(int32 At=0;At+VersionTagSize<=Untagged.Num();++At)
    {if(FMemory::Memcmp(Untagged.GetData()+At,VersionTag,VersionTagSize)==0){bTagged=true;break;}}
    TestFalse(TEXT("This regression fixture really omits the default-valued Version property tag"),bTagged);
    if(!UGameplayStatics::SaveGameToSlot(Save,Slot,0)){return false;}
    F.Player->Destroy();F.World->URL=FURL(nullptr,*LegacyLoad,TRAVEL_Absolute);F.SpawnPlayer();
    if(!F.Valid()){return false;}F.Player->Tick(0.0f);
    TestTrue(TEXT("The production reader migrates an actual untagged legacy save instead of treating it as version4"),
        F.Player->HasRestoredWadCheckpoint()&&F.Data(0,373,false,0)
        &&F.Data(F.Player->GetReadySlot(),373,false,0)&&F.Data(F.Player->GetMouseHandSlot(),37569,false,0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedMetadataPreflight,"AAOriginal.WAD.Inventory.CarriedMetadataPreflight",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedMetadataPreflight::RunTest(const FString& Parameters)
{
    using namespace AAOriginalCarriedRuntimeTests;
    const FString Prefix=TEXT("/Game/Maps/L30?WadCreated=1?WadRuneRules=1?WadItems=373,37569?");
    const auto Check=[&](const TCHAR* Payload){return NativeWadCarriedObjectState::ValidateOptions(FURL(nullptr,*(Prefix+Payload),TRAVEL_Absolute));};
    TestTrue(TEXT("A truly absent legacy field is allowed"),Check(TEXT("")));
    for(const TCHAR* Bad:{TEXT("WadObjectData"),TEXT("WadObjectData="),TEXT("WadObjectData=1;2;373:1:407,705:1:0"),TEXT("WadObjectData=1;2;373:0:407,37569:1:0")})
    {TestFalse(TEXT("Malformed modern metadata is not reinterpreted as legacy"),Check(Bad));}
    TestTrue(TEXT("Exact full-type modern metadata passes"),Check(TEXT("WadObjectData=1;2;373:1:407,37569:1:0")));
    FCarriedDataFixture F(nullptr,false,true);if(!F.Valid()){return false;}
    if(!F.Player->CreateWadItemInMouseHandWithAccessoryData(373,407)){return false;}
    const FString Slot=TEXT("CarriedMetadataAtomic-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT{UGameplayStatics::DeleteGameInSlot(Slot,0);};
    if(!F.Player->SaveWadCheckpoint(Slot)){return false;}
    auto* Save=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));if(!Save){return false;}
    Save->PlayerOptions.ReplaceInline(TEXT("373:1:407"),TEXT("999:1:407"));Save->Position={222,333,444};
    if(!UGameplayStatics::SaveGameToSlot(Save,Slot,0)){return false;}
    F.Player->Destroy();
    const FString Initial=FString::Printf(TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode?WadClass=4?WadCreated=1?WadRuneRules=1?WadItems=201?WadReady=0?WadHealth=1234?WadLoad=%s"),*Slot);
    F.World->URL=FURL(nullptr,*Initial,TRAVEL_Absolute);F.SpawnPlayer();if(!F.Valid()){return false;}
    const auto Before=F.Player->GetCarriedItems();const int32 H=F.Player->GetHealth();const FVector Position=F.Player->GetActorLocation();
    AddExpectedError(TEXT("No compatible WAD checkpoint for this map"),EAutomationExpectedMessageFlags::Contains,1);
    F.Player->Tick(0.0f);
    TestFalse(TEXT("The reader refuses malformed metadata before restoring the checkpoint"),F.Player->HasRestoredWadCheckpoint());
    TestTrue(TEXT("Rejected checkpoint preserves existing ownership and legacy metadata"),F.Player->GetCarriedItems()==Before&&F.Data(0,201,false,0));
    TestTrue(TEXT("Rejected checkpoint preserves live vitals and position"),F.Player->GetHealth()==H&&F.Player->GetActorLocation().Equals(Position));
    FURL Missing(nullptr,*(Save->LevelPackage+TEXT("?")+Save->PlayerOptions),TRAVEL_Absolute);Missing.RemoveOption(TEXT("WadObjectData"));
    const FString MissingURL=Missing.ToString();Save->PlayerOptions=MissingURL.Mid(MissingURL.Find(TEXT("?"))+1);
    if(!UGameplayStatics::SaveGameToSlot(Save,Slot,0)){return false;}
    F.Player->Destroy();F.World->URL=FURL(nullptr,*Initial,TRAVEL_Absolute);F.SpawnPlayer();
    AddExpectedError(TEXT("No compatible WAD checkpoint for this map"),EAutomationExpectedMessageFlags::Contains,1);
    F.Player->Tick(0.0f);
    TestTrue(TEXT("Version4 omission is not permitted to masquerade as an older legacy save"),!F.Player->HasRestoredWadCheckpoint()&&F.Player->GetCarriedItems()==Before&&F.Data(0,201,false,0));
    // Direct malformed travel also preserves every int owner, but refuses item
    // mutation and saving rather than erase or guess its corrupted modern data.
    F.Player->Destroy();F.World->URL=FURL(nullptr,*(Initial+TEXT("?WadObjectData=broken")),TRAVEL_Absolute);F.SpawnPlayer();
    TestFalse(TEXT("Malformed direct-travel metadata stays invalid"),F.Player->HasValidCarriedObjectData());
    TestEqual(TEXT("Invalid direct travel retains the physical item"),F.Player->GetItemCount(201),1);
    TestFalse(TEXT("Invalid direct travel cannot consume an untracked owner"),F.Player->ConsumeItem(201));
    TestFalse(TEXT("Invalid direct travel cannot erase the prior checkpoint"),F.Player->SaveWadCheckpoint(Slot));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedMetadataTown,"AAOriginal.WAD.Inventory.CarriedMetadataTown",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedMetadataTown::RunTest(const FString& Parameters)
{
    using namespace AAOriginalCarriedRuntimeTests;
    const TCHAR* Options=TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode?WadClass=4?WadCreated=0?WadRuneRules=1?WadReady=-1?WadItems=373?WadObjectData=1;1;373:1:407");
    FCarriedDataFixture F(Options,false,true);if(!F.Valid()){return false;}
    if(!TestTrue(TEXT("Normal first-time class creation can append its source starter objects"),F.Player->ChooseWadClass(NativeWadRules::PriestClass))){return false;}
    TestTrue(TEXT("Appending starter items never reinitializes a preexisting physical payload"),F.Data(0,373,true,407));
    for(int32 Slot=1;Slot<F.Player->GetCarriedItems().Num();++Slot)
    {const auto* Data=F.Player->GetCarriedObjectData(Slot);TestTrue(TEXT("Each new source starter owns an aligned default payload"),Data&&Data->Type==F.Player->GetCarriedItems()[Slot]&&!Data->bAccessoryDataRecorded&&Data->AccessoryData==0);}
    auto* Catalog=static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr,UNativeWadHudData::ObjectPath));
    auto* Town=static_cast<UNativeWadTownData*>(LoadObject<UObject>(nullptr,TEXT("/Game/Maps/WadTown_Assets/Town/TownData.TownData")));
    UClass* ContentClass=FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadTownContent"));
    auto* Content=ContentClass?static_cast<ANativeWadTownContent*>(F.World->SpawnActor<AActor>(ContentClass)):nullptr;
    auto* Travel=F.Travel();
    if(!Catalog||!Town||!Content||!Travel){return false;}Content->Data=Town;
    constexpr int32 Type=426;const auto* Rules=Catalog->ItemDetails.Find(Type);const int32* Tier=Town->BankStock.Find(Type);
    if(!Rules||!Tier||Rules->ValueCopper<=0){return false;}Travel->Quest.ShopTier=*Tier;
    const int32 Before=F.Player->GetCarriedItems().Num();Travel->Quest.SetWalletCopper(0);
    TestFalse(TEXT("A refused source Bank purchase appends no item or metadata"),F.Player->TradeWadTownItem(Type,false,true));
    TestEqual(TEXT("Refused purchase preserves physical count"),F.Player->GetCarriedItems().Num(),Before);
    TestTrue(TEXT("Refused purchase preserves another exact payload"),F.Data(0,373,true,407));
    Travel->Quest.SetWalletCopper(Rules->ValueCopper);
    if(!TestTrue(TEXT("A successful actual Bank transaction creates one retained owner"),F.Player->TradeWadTownItem(Type,false,true))){return false;}
    const int32 Bought=F.Player->GetMouseHandSlot();
    TestTrue(TEXT("Purchased owner has its own aligned source-default metadata"),F.Data(Bought,Type,false,0));
    if(!TestTrue(TEXT("Actual Bank selling removes that exact metadata row"),F.Player->TradeWadTownItem(Bought,true,true))){return false;}
    TestTrue(TEXT("Sale remapping preserves the original physical page and valid ownership"),F.Data(0,373,true,407)&&F.Player->HasValidCarriedObjectData());
    TestEqual(TEXT("Buy/sell roundtrip leaves the original item count"),F.Player->GetCarriedItems().Num(),Before);
    return true;
}
