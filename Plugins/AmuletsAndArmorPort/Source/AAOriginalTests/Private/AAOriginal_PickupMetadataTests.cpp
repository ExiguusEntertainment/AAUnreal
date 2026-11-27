#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadEncounter.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadPickupMetadata.h"

namespace
{
    struct FPickupMetadataFixture
    {
        UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        APlayerController* Controller = nullptr;
        ANativeWadHUD* Hud = nullptr;
        UNativeWadTravelState* Travel = nullptr;
        UInputComponent* Input = nullptr;
        FPickupMetadataFixture()
        {
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            UClass* TravelClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadTravelState"));
            Travel = TravelClass ? static_cast<UNativeWadTravelState*>(Instance->GetSubsystemBase(TravelClass)) : nullptr;
            Player = World->SpawnActor<ANativeWadPlayer>();
            Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Controller->SetControlRotation(FRotator::ZeroRotator);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
            UClass* HudClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadHUD"));
            if (HudClass)
            {
                // An isolated controller has no owning connection for the client RPC wrapper.
                // Initialize its real HUD through the same local implementation.
                Controller->ClientSetHUD_Implementation(HudClass);
                Hud = static_cast<ANativeWadHUD*>(Controller->GetHUD());
                if (Hud && !Hud->IsActorInitialized()) { Hud->PostInitializeComponents(); }
                if (Hud && !Hud->HasActorBegunPlay()) { Hud->DispatchBeginPlay(); }
            }
        }
        ~FPickupMetadataFixture()
        {
            Instance->Shutdown();
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        void CloseBanner()
        {
            if (!Player || !Input || !Player->IsInventoryOpen()) { return; }
            for (const FInputKeyBinding& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(EKeys::I))
                { Binding.KeyDelegate.Execute(EKeys::I); return; }
            }
        }
        ANativeWadPickup* Spawn(int32 Type)
        {
            CloseBanner();
            FActorSpawnParameters Parameters;
            Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto* Pickup = World->SpawnActor<ANativeWadPickup>(Player->GetPawnViewLocation() + FVector(30, 0, 0),
                FRotator::ZeroRotator, Parameters);
            if (Pickup) { Pickup->Configure(nullptr, nullptr, Type); }
            return Pickup;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSharedJournalCatalog, "AAOriginal.WAD.Interface.SharedJournalCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSharedJournalCatalog::RunTest(const FString& Parameters)
{
    int32 Page = 99;
    for (const FString Name : {TEXT("NOTE0000"), TEXT("NOTE0407"), TEXT("NOTE0672"), TEXT("NOTE1023")})
    { TestTrue(TEXT("A finished original journal name parses"), NativeWadPickupMetadata::ParseJournalPageName(Name, Page)); }
    for (const FString Name : {TEXT("NOTE0407.PIC"), TEXT("NOTE1024"), TEXT("NOTE-001"), TEXT("NOTE407"), TEXT("note0407"), TEXT("NOTE04x7")})
    { TestFalse(TEXT("Authoring copies and invalid page identities are excluded"), NativeWadPickupMetadata::ParseJournalPageName(Name, Page)); }
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/UI/NOTES/NOTE*")), true, false);
    TSet<int32> SourcePages;
    for (const FString& Name : Files)
    { if (NativeWadPickupMetadata::ParseJournalPageName(Name, Page)) { SourcePages.Add(Page); } }
    TestEqual(TEXT("The source contains 259 finished top-level journal resources"), SourcePages.Num(), 259);
    const auto* Catalog = static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr, UNativeWadHudData::ObjectPath));
    if (!TestNotNull(TEXT("Saved classic HUD catalog"), Catalog)) { return false; }
    TestEqual(TEXT("Every finished page is saved independently of map script references"), Catalog->JournalPageCatalog.Num(), SourcePages.Num());
    for (int32 Id : SourcePages)
    { TestNotNull(TEXT("A source page has a saved runtime texture"), Catalog->JournalPageCatalog.FindRef(Id).Get()); }
    for (int32 Id : {0, 407, 672})
    { TestNotNull(TEXT("General source/treasure journal pages are present"), Catalog->JournalPageCatalog.FindRef(Id).Get()); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalJournalPickupGet, "AAOriginal.WAD.Gameplay.JournalPickupGet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalJournalPickupGet::RunTest(const FString& Parameters)
{
    FPickupMetadataFixture Fixture;
    if (!Fixture.Player || !TestNotNull(TEXT("Character journal state"), Fixture.Travel)
        || !TestNotNull(TEXT("Original journal HUD"), Fixture.Hud)) { return false; }
    auto& Player = *Fixture.Player;
    Player.InitializeItemIdentification(false);
    Player.CollectItem(201);
    Player.TakeInventoryItem(Player.GetCarriedItems().Num() - 1);
    int32 ExpectedPages = 0;
    for (const int32 Id : {407, 672, 407, 0})
    {
        auto* Pickup = Fixture.Spawn(373);
        if (!Pickup || !Pickup->SetAccessoryData(Id)) { return false; }
        const bool bDuplicate = Fixture.Travel->JournalPages.Contains(Id);
        if (!bDuplicate) { ++ExpectedPages; }
        if (!TestTrue(TEXT("Normal gameplay Use executes the world scroll GET trigger"), Player.Use())) { return false; }
        TestEqual(TEXT("Only exact unique page identities are retained"), Fixture.Travel->JournalPages.Num(), ExpectedPages);
        TestEqual(TEXT("The exact acquired source page opens automatically, including page0"), Fixture.Hud->GetJournalPageId(), Id);
        TestEqual(TEXT("A GET-destroyed journal scroll never enters carried inventory"), Player.GetItemCount(373), 0);
        TestEqual(TEXT("A source GET page does not replace an already held object"), Player.GetMouseHandItem(), 201);
        TestEqual(TEXT("The original source feedback distinguishes duplicate pages"), Player.GetInteractionMessage(),
            FString(bDuplicate ? TEXT("You already had a copy of this note.") : TEXT("You found a journal entry")));
        TestFalse(TEXT("The same world actor cannot execute its GET twice"), Pickup->Collect(Player));
    }
    for (const int32 Invalid : {1024, 65535})
    {
        auto* Pickup = Fixture.Spawn(373);
        if (!Pickup || !Pickup->SetAccessoryData(Invalid)) { return false; }
        TestFalse(TEXT("Invalid source journal identity is refused without an assertion"), Player.Use());
        TestTrue(TEXT("Unreadable source data is not silently consumed or converted into page0"), Pickup->IsAvailable() && !Pickup->IsActorBeingDestroyed());
        TestEqual(TEXT("Refused data adds no extra journal page"), Fixture.Travel->JournalPages.Num(), ExpectedPages);
        Pickup->Destroy();
    }
    auto* UnknownLegacy = Fixture.Spawn(373);
    if (!UnknownLegacy) { return false; }
    TestFalse(TEXT("An old physical journal object with lost auxiliary data does not guess a page identity"), Player.Use());
    TestEqual(TEXT("The legacy refusal preserves known pages"), Fixture.Travel->JournalPages.Num(), ExpectedPages);
    TestEqual(TEXT("No journal GET case has added a carried373 object"), Player.GetItemCount(373), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPickupAuxiliarySave, "AAOriginal.WAD.Gameplay.PickupAuxiliarySave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalPickupAuxiliarySave::RunTest(const FString& Parameters)
{
    FPickupMetadataFixture Fixture;
    if (!Fixture.Player) { return false; }
    UClass* SaveClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadCheckpoint"));
    if (!TestNotNull(TEXT("Reflected native checkpoint class"), SaveClass)) { return false; }
    auto* Save = static_cast<UNativeWadCheckpoint*>(NewObject<USaveGame>(GetTransientPackage(), SaveClass));
    for (const auto& Data : {FWadSavedDrop{144, FVector::ZeroVector, 0, 6},
        FWadSavedDrop{144, FVector::ZeroVector, 0, 12, true, 6}, FWadSavedDrop{144, FVector::ZeroVector, 0, 6, true, 0},
        FWadSavedDrop{373, FVector::ZeroVector, INDEX_NONE, 0, true, 407}})
    {
        auto* Source = Fixture.Spawn(Data.Type);
        if (!Source) { return false; }
        NativeWadPickupMetadata::RestoreSavedDrop(Data, *Source);
        // Exercise the same actor snapshot writer used by the native level checkpoint.
        NativeWadPickupMetadata::WriteSavedDrop(*Source, Save->Level.DroppedItems.AddDefaulted_GetRef());
        Source->Destroy();
    }
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("Real SaveGame serialization writes legacy and source payloads"), UGameplayStatics::SaveGameToMemory(Save, Bytes))) { return false; }
    auto* Loaded = static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromMemory(Bytes));
    if (!TestNotNull(TEXT("Checkpoint readback"), Loaded)) { return false; }
    TestEqual(TEXT("All saved drop records remain independent"), Loaded->Level.DroppedItems.Num(), 4);
    if (Loaded->Level.DroppedItems.Num() != 4) { return false; }
    TestFalse(TEXT("The earlier native partial-quiver format keeps its absent source flag"), Loaded->Level.DroppedItems[0].bAccessoryDataRecorded);
    TestTrue(TEXT("Snapshot and SaveGame readback retain an explicit six-unit source payload"),
        Loaded->Level.DroppedItems[1].bAccessoryDataRecorded && Loaded->Level.DroppedItems[1].AccessoryData == 6);
    TestTrue(TEXT("Snapshot and SaveGame readback distinguish recorded zero from absent data"),
        Loaded->Level.DroppedItems[2].bAccessoryDataRecorded && Loaded->Level.DroppedItems[2].AccessoryData == 0);
    const int32 Quantities[] = {6, 6, 12};
    int32 Total = 0;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        auto* Pickup = Fixture.Spawn(Loaded->Level.DroppedItems[Index].Type);
        if (!Pickup) { return false; }
        NativeWadPickupMetadata::RestoreSavedDrop(Loaded->Level.DroppedItems[Index], *Pickup);
        TestEqual(TEXT("Source payload overrides stale native quantities; absent payload keeps old partial quivers"), Pickup->GetAmmoQuantity(), Quantities[Index]);
        if (!TestTrue(TEXT("Normal Use consumes the restored quiver once"), Fixture.Player->Use())) { return false; }
        Total += Quantities[Index];
        TestEqual(TEXT("Only the restored source/legacy quantity is added"), Fixture.Player->GetAmmoReserve(0), Total);
        TestFalse(TEXT("A consumed quiver cannot grant its quantity twice"), Pickup->Collect(*Fixture.Player));
    }
    auto* Page = Fixture.Spawn(373);
    if (!Page) { return false; }
    NativeWadPickupMetadata::RestoreSavedDrop(Loaded->Level.DroppedItems[3], *Page);
    TestTrue(TEXT("The saved source page retains its presence flag and exact uint16 payload"), Page->HasAccessoryData() && Page->GetAccessoryData() == 407);
    if (!TestTrue(TEXT("The restored journal drop executes GET"), Fixture.Player->Use())) { return false; }
    TestTrue(TEXT("Restored page407 is stored in the character journal"), Fixture.Travel && Fixture.Travel->JournalPages.Contains(407));
    TestEqual(TEXT("Restored GET also does not add373 to the bag"), Fixture.Player->GetItemCount(373), 0);

    Save->JournalPages = Fixture.Travel->JournalPages;
    Bytes.Reset();
    if (!TestTrue(TEXT("Character journal pictures serialize separately from world drops"), UGameplayStatics::SaveGameToMemory(Save, Bytes))) { return false; }
    auto* JournalReadback = static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromMemory(Bytes));
    if (!TestNotNull(TEXT("Character journal readback"), JournalReadback)) { return false; }
    const auto* SavedPicture = JournalReadback->JournalPages.Find(407);
    TestTrue(TEXT("The acquired page retains its saved shared asset reference without a map-specific page table"),
        SavedPicture && SavedPicture->ToSoftObjectPath().ToString().StartsWith(TEXT("/Game/WadInterface/Journal/")));

    auto* Bolt = Fixture.Spawn(206);
    if (!Bolt || !Bolt->SetAccessoryData(0)) { return false; }
    TestFalse(TEXT("Out-of-range accessory setters reject negative data"), Bolt->SetAccessoryData(-1));
    TestFalse(TEXT("Out-of-range accessory setters reject values above uint16"), Bolt->SetAccessoryData(65536));
    TestTrue(TEXT("Refused setters preserve the recorded zero/default payload"), Bolt->HasAccessoryData() && Bolt->GetAccessoryData() == 0);
    if (!TestTrue(TEXT("A zero-aux source bolt grants its original one-bolt default"), Fixture.Player->Use())) { return false; }
    TestEqual(TEXT("Quiver and bolt defaults remain distinct"), Fixture.Player->GetAmmoReserve(0), Total + 1);
    return true;
}
