#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadBagState.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalModernBagPreflight,"AAOriginal.WAD.Inventory.ModernBagPreflight",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalModernBagPreflight::RunTest(const FString& Parameters)
{
    const auto Rule=[](int32 Type){NativeWadBagState::FItemRule R;R.Item={Type,{7,6},1,false};return R;};
    const FString Prefix=TEXT("/Game/Maps/L30?WadClass=1?WadRuneRules=1?WadItems=435?WadReady=-1?WadMouseHand=-1?");
    const auto Check=[&](const TCHAR* S){return NativeWadBagState::ValidateOptions(FURL(nullptr,*(Prefix+S),TRAVEL_Absolute),Rule);};
    TestTrue(TEXT("Absent legacy layout remains ownership-preserving migration"),Check(TEXT("")));
    TestFalse(TEXT("Present empty modern layout is not legacy"),Check(TEXT("WadBagLayout=")));
    TestFalse(TEXT("Present bare modern marker is not legacy"),Check(TEXT("WadBagLayout")));
    TestTrue(TEXT("Complete modern ownership and layout validate"),Check(TEXT("WadBagLayout=1,5,0,1~435,0,5,7,0,0?WadBagPage=0")));
    TestFalse(TEXT("Modern type witness mismatch refuses before runtime repair"),Check(TEXT("WadBagLayout=1,5,0,1~436,0,5,7,0,0")));
    TestFalse(TEXT("Bare malformed modern page is not a missing legacy default"),Check(TEXT("WadBagLayout=1,5,0,1~435,0,5,7,0,0?WadBagPage")));
    TestFalse(TEXT("Modern visible page must exist"),Check(TEXT("WadBagLayout=1,5,0,1~435,0,5,7,0,0?WadBagPage=5")));
    TestFalse(TEXT("An independently held object cannot simultaneously occupy a bag footprint"),
        NativeWadBagState::ValidateOptions(FURL(nullptr,*(Prefix+TEXT("WadBagLayout=1,5,0,1~435,0,5,7,0,0?WadMouseHand=0")),TRAVEL_Absolute),Rule));
    const FString Double=TEXT("/Game/Maps/L30?WadClass=1?WadRuneRules=1?WadItems=435?WadReady=0?WadMouseHand=0?WadBagLayout=1,5,0,1~435,-1,-1,-1,-1,0");
    TestFalse(TEXT("Duplicate ready/mouse ownership refuses"),NativeWadBagState::ValidateOptions(FURL(nullptr,*Double,TRAVEL_Absolute),Rule));
    const FString Partial=TEXT("/Game/Maps/L30?WadClass=1?WadRuneRules=1?WadItems=435junk?WadBagLayout=1,5,0,1~435,0,5,7,0,0");
    TestFalse(TEXT("Modern carried types require whole canonical tokens"),NativeWadBagState::ValidateOptions(FURL(nullptr,*Partial,TRAVEL_Absolute),Rule));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBagCheckpointAtomicity,"AAOriginal.WAD.Inventory.BagCheckpointAtomicity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalBagCheckpointAtomicity::RunTest(const FString& Parameters)
{
    UGameInstance* Instance=NewObject<UGameInstance>(GEngine);Instance->InitializeStandalone();UWorld* World=Instance->GetWorld();
    constexpr const TCHAR* Slot=TEXT("NativeWadBagCorruptionUnit");
    ON_SCOPE_EXIT
    {UGameplayStatics::DeleteGameInSlot(Slot,0);Instance->Shutdown();if(World){GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}};
    if(!World){return false;}
    World->URL=FURL(nullptr,TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode?WadClass=1?WadCreated=1?WadRuneRules=1?WadItems=712,712?WadReady=-1"),TRAVEL_Absolute);
    if(!World->SetGameMode(World->URL)){return false;}
    auto* Controller=World->SpawnActor<APlayerController>();auto* P=World->SpawnActor<ANativeWadPlayer>();
    if(!Controller||!P){return false;}World->AddController(Controller);Controller->Possess(P);P->DispatchBeginPlay();
    if(!TestTrue(TEXT("Write an actual valid baseline checkpoint"),P->SaveWadCheckpoint(Slot))){return false;}
    auto* Saved=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    if(!Saved){return false;}const FString OriginalOptions=Saved->PlayerOptions;const auto OriginalLayout=P->GetBagLayout();
    // Deliberately corrupt only private placement invariants through the public pure
    // model in this test: two tiny footprints cannot describe actual64x64 shields.
    auto FakeItems=P->GetBagItems();for(auto& Item:FakeItems){Item.Bitmap={7,6};}
    FWadBagLayout Fake;if(!Fake.Initialize(FakeItems,false)){return false;}
    const_cast<FWadBagLayout&>(P->GetBagLayout())=Fake;
    TestFalse(TEXT("Production save refuses invalid/unencodable current bag state"),P->SaveWadCheckpoint(Slot));
    const auto* Unchanged=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot,0));
    TestTrue(TEXT("Refused save leaves the previous disk checkpoint intact"),Unchanged&&Unchanged->PlayerOptions==OriginalOptions);
    const_cast<FWadBagLayout&>(P->GetBagLayout())=OriginalLayout;
    const FString Good=OriginalLayout.Encode(P->GetBagItems());FString Bad=Good;
    Bad.ReplaceInline(TEXT("~712,"),TEXT("~999,"));Saved->PlayerOptions.ReplaceInline(*Good,*Bad);
    Saved->Position=FVector(222,333,444);
    if(!UGameplayStatics::SaveGameToSlot(Saved,Slot,0)){return false;}
    P->Destroy();
    const FString Initial=FString::Printf(TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode?WadClass=1?WadCreated=1?WadRuneRules=1?WadItems=201?WadReady=0?WadHealth=1234?WadLoad=%s"),Slot);
    World->URL=FURL(nullptr,*Initial,TRAVEL_Absolute);P=World->SpawnActor<ANativeWadPlayer>();Controller->Possess(P);P->DispatchBeginPlay();
    const auto BeforeItems=P->GetCarriedItems();const int32 BeforeHealth=P->GetHealth();const FVector BeforePosition=P->GetActorLocation();
    const FString BeforeLayout=P->GetBagLayout().Encode(P->GetBagItems());
    AddExpectedError(TEXT("No compatible WAD checkpoint for this map"),EAutomationExpectedMessageFlags::Contains,1);
    P->Tick(0.0f);
    TestFalse(TEXT("Malformed modern checkpoint was not accepted"),P->HasRestoredWadCheckpoint());
    TestTrue(TEXT("Checkpoint reader rejects before replacing live owned items"),P->GetCarriedItems()==BeforeItems);
    TestEqual(TEXT("Reader rejection preserves live vitals"),P->GetHealth(),BeforeHealth);
    TestTrue(TEXT("Reader rejection preserves live position"),P->GetActorLocation().Equals(BeforePosition));
    TestEqual(TEXT("Reader rejection preserves live spatial placement; no legacy overflow repair"),P->GetBagLayout().Encode(P->GetBagItems()),BeforeLayout);
    const FString Legacy=TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode?WadClass=1?WadCreated=1?WadRuneRules=1?WadItems=712,712,712,712,712,712?WadReady=-1");
    P->Destroy();World->URL=FURL(nullptr,*Legacy,TRAVEL_Absolute);P=World->SpawnActor<ANativeWadPlayer>();Controller->Possess(P);P->DispatchBeginPlay();
    TestEqual(TEXT("Missing old field migrates all six original large items"),P->GetItemCount(712),6);
    TestEqual(TEXT("Only genuine missing legacy field receives necessary overflow"),P->GetBagLayout().PageCount(),6);
    TestTrue(TEXT("Legacy exception is explicitly recorded"),P->GetBagLayout().HasLegacyOverflow());
    P->Destroy();World->URL=FURL(nullptr,*(Legacy+TEXT("?WadBagLayout=malformed")),TRAVEL_Absolute);
    P=World->SpawnActor<ANativeWadPlayer>();Controller->Possess(P);
    AddExpectedError(TEXT("Could not place owned inventory within five source bags"),EAutomationExpectedMessageFlags::Contains,1);
    P->DispatchBeginPlay();
    TestEqual(TEXT("Malformed direct modern travel retains all physical ownership"),P->GetItemCount(712),6);
    TestFalse(TEXT("Malformed modern data cannot grant legacy overflow"),P->GetBagLayout().HasLegacyOverflow());
    TestEqual(TEXT("Repeated getter/repair does not reinterpret corruption as missing legacy"),P->GetBagLayout().PageCount(),5);
    TestFalse(TEXT("Unrepresentable malformed direct travel cannot overwrite a checkpoint"),P->SaveWadCheckpoint(Slot));
    return true;
}
