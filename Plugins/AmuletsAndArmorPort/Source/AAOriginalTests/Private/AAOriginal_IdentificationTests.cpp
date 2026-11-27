#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadTown.h"

namespace
{
    struct FIdentificationFixture
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        ANativeWadPlayer* Player = World ? World->SpawnActor<ANativeWadPlayer>() : nullptr;
        UInputComponent* Input = Player ? NewObject<UInputComponent>(Player) : nullptr;
        FIdentificationFixture() { if (Player) { Player->SetupPlayerInputComponent(Input); } }
        ~FIdentificationFixture() { if (World) { World->DestroyWorld(false); } }
        bool Press(FKey Key)
        {
            if (!Input) { return false; }
            for (const FInputKeyBinding& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }
        bool Ready(int32 Type)
        {
            if (!Press(EKeys::I)) { return false; }
            Player->StoreMouseHandInInventory();
            Player->ClickInventorySlot(Player->GetCarriedItems().Find(Type));
            Player->ClickInventorySlot(INDEX_NONE);
            return Press(EKeys::I) && Player->GetReadyItem() == Type;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalItemKnowledge, "AAOriginal.WAD.Gameplay.ItemKnowledge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalItemKnowledge::RunTest(const FString& Parameters)
{
    FIdentificationFixture Fixture;
    auto* Player = Fixture.Player;
    if (!TestNotNull(TEXT("Player for source identification"), Player)) { return false; }
    Player->InitializeItemIdentification(false);
    FString Unknown, Known;
    const FString Source = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
    if (!TestTrue(TEXT("Original unidentified ring text exists"),
        FFileHelper::LoadFileToString(Unknown, *(Source / TEXT("OBJDESC/DES00417.TXT"))))) { return false; }
    if (!TestTrue(TEXT("Original identified ring text exists"),
        FFileHelper::LoadFileToString(Known, *(Source / TEXT("OBJDESC2/DES00417.TXT"))))) { return false; }
    Unknown.TrimStartAndEndInline();
    Known.TrimStartAndEndInline();
    Player->CollectItem(417);
    Player->CollectItem(417);
    TestEqual(TEXT("Unidentified objects display the source OBJDESC text"), Player->GetInventoryItemDescription(417), Unknown);
    TestEqual(TEXT("Merchant previews display OBJDESC2"), Player->GetInventoryItemDescription(417, true), Known);
    TestFalse(TEXT("A merchant preview does not teach the character"), Player->HasIdentifiedItemType(417));
    TestTrue(TEXT("Learning a new full type reports a change"), Player->IdentifyItemType(417));
    TestFalse(TEXT("The same full type is learned only once"), Player->IdentifyItemType(417));
    TestEqual(TEXT("Both duplicate objects now share the source identified description"), Player->GetInventoryItemDescription(417), Known);
    Player->ConsumeItem(417);
    Player->ConsumeItem(417);
    TestTrue(TEXT("Discovery survives the last object being removed"), Player->HasIdentifiedItemType(417));
    Player->IdentifyItemType(200);
    TestFalse(TEXT("A packed variant is independent of its base type"), Player->HasIdentifiedItemType(32968));
    TestTrue(TEXT("A full packed variant can be learned"), Player->IdentifyItemType(32968));
    TestTrue(TEXT("The original uint16 upper limit is valid"), Player->IdentifyItemType(65535));
    for (int32 Invalid : {-1, 0, 65536})
    { TestFalse(TEXT("Out-of-range identities are rejected"), Player->IdentifyItemType(Invalid)); }

    Player->CollectItem(200);
    Player->CollectItem(32968);
    const TArray<int32> Before = Player->GetCarriedItems();
    Player->RestoreItemIdentification({}, false);
    TestTrue(TEXT("A legacy save retains names of its carried base type"), Player->HasIdentifiedItemType(200));
    TestTrue(TEXT("Legacy migration retains the exact carried packed type"), Player->HasIdentifiedItemType(32968));
    TestFalse(TEXT("Legacy migration does not guess historical removed types"), Player->HasIdentifiedItemType(417));
    TestTrue(TEXT("Legacy migration grants no objects or starter scroll"), Before == Player->GetCarriedItems());
    Player->RestoreItemIdentification({}, true);
    TestFalse(TEXT("A recorded empty knowledge set remains empty"), Player->HasIdentifiedItemType(200));
    Player->RestoreItemIdentification(TSet<int32>{200, 32968, 0, 65536}, true);
    TestEqual(TEXT("Recorded state validates full identities without collapsing variants"), Player->GetIdentifiedItemTypes().Num(), 2);
    // Restoring from the character's own set must not invalidate the input while replacing it.
    Player->RestoreItemIdentification(Player->GetIdentifiedItemTypes(), true);
    TestTrue(TEXT("Aliased restoration preserves both exact identities"),
        Player->HasIdentifiedItemType(200) && Player->HasIdentifiedItemType(32968));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalIdentificationUse, "AAOriginal.WAD.Gameplay.IdentificationUse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalIdentificationUse::RunTest(const FString& Parameters)
{
    FIdentificationFixture Fixture;
    auto* Player = Fixture.Player;
    if (!TestNotNull(TEXT("Player for ordinary identification input"), Player)) { return false; }
    Player->InitializeItemIdentification(false);
    Player->CollectItem(417);
    Player->CollectItem(807);
    Player->CollectItem(350);
    Fixture.Press(EKeys::I);
    Player->ClickInventorySlot(Player->GetCarriedItems().Find(417));
    Player->ClickJewelrySlot(NativeWadRules::Ring1Slot);
    Fixture.Press(EKeys::I);
    TestFalse(TEXT("Equipping an unidentified ring does not reveal its name"), Player->HasIdentifiedItemType(417));
    if (!TestTrue(TEXT("The source equipped-identification scroll can be readied"), Fixture.Ready(350))) { return false; }
    Fixture.Press(EKeys::LeftControl);
    TestEqual(TEXT("The equipped scroll is consumed once by normal use"), Player->GetItemCount(350), 0);
    TestTrue(TEXT("The source scroll identifies worn jewelry"), Player->HasIdentifiedItemType(417));
    TestTrue(TEXT("Reading a scroll identifies its own type"), Player->HasIdentifiedItemType(350));
    TestFalse(TEXT("Equipped identification leaves bag-only potions unidentified"), Player->HasIdentifiedItemType(807));
    TestEqual(TEXT("The source equipped-identification message is retained"), Player->GetInteractionMessage(),
        FString(TEXT("You understand your equipment better!")));
    Player->ReceiveTrapPoison(50);
    if (!TestTrue(TEXT("The still-unknown cure can be readied"), Fixture.Ready(807))) { return false; }
    Fixture.Press(EKeys::LeftControl);
    TestEqual(TEXT("Normal potion use still cures poison"), Player->GetPoisonLevel(), 0);
    TestEqual(TEXT("Normal potion use consumes it exactly once"), Player->GetItemCount(807), 0);
    TestTrue(TEXT("The successful source potion use teaches its full type"), Player->HasIdentifiedItemType(807));
    Player->CollectItem(806);
    Player->CollectItem(351);
    if (!TestTrue(TEXT("The source all-identification scroll can be readied"), Fixture.Ready(351))) { return false; }
    Fixture.Press(EKeys::LeftControl);
    TestEqual(TEXT("The all-identification scroll is consumed once"), Player->GetItemCount(351), 0);
    TestTrue(TEXT("All-identification teaches a bag-only item and its own consumed type"),
        Player->HasIdentifiedItemType(806) && Player->HasIdentifiedItemType(351));
    TestTrue(TEXT("The previously consumed potion stays known"), Player->HasIdentifiedItemType(807));
    TestEqual(TEXT("The source inventory-identification message is retained"), Player->GetInteractionMessage(),
        FString(TEXT("You understand your inventory better!")));
    Fixture.Press(EKeys::LeftControl);
    TestEqual(TEXT("An empty ready hand cannot read the consumed scroll again"), Player->GetItemCount(351), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalStarterIdentification, "AAOriginal.WAD.Gameplay.StarterIdentification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalStarterIdentification::RunTest(const FString& Parameters)
{
    UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
    // Give subsystem initialization a real WorldContext/GetWorld, matching normal standalone startup.
    Instance->InitializeStandalone();
    UWorld* World = Instance->GetWorld();
    ON_SCOPE_EXIT
    {
        Instance->Shutdown();
        if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
    };
    if (!TestNotNull(TEXT("World for actual new-character creation"), World)) { return false; }
    UClass* TravelClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadTravelState"));
    // The concrete subsystem is private to the runtime module; resolve its reflected class without
    // importing a non-exported StaticClass symbol into the separate test DLL.
    auto* Travel = TravelClass ? static_cast<UNativeWadTravelState*>(Instance->GetSubsystemBase(TravelClass)) : nullptr;
    if (!TestNotNull(TEXT("Initialized per-character travel state"), Travel)) { return false; }
    const FURL URL(nullptr, TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"), TRAVEL_Absolute);
    if (!TestTrue(TEXT("Use the actual native Town game mode"), World->SetGameMode(URL))) { return false; }
    auto* Player = World->SpawnActor<ANativeWadPlayer>();
    if (!TestNotNull(TEXT("Fresh native character"), Player)) { return false; }
    if (!TestTrue(TEXT("Fresh Town character can choose the source default class"), Player->ChooseWadClass(Player->GetCharacterClass()))) { return false; }
    TestEqual(TEXT("Original common creation grants four gold"), Travel->Quest.GetWalletCopper(), int64(400));
    TestEqual(TEXT("Original common creation grants one Identify All scroll"), Player->GetItemCount(351), 1);
    for (const int32 Type : Player->GetCarriedItems())
    { TestTrue(TEXT("Every fresh starter type is known"), Player->HasIdentifiedItemType(Type)); }
    TestTrue(TEXT("Creation records knowledge for normal departure"), Travel->bItemIdentificationRecorded);
    TestTrue(TEXT("Creation synchronizes the exact learned types"), Travel->IdentifiedItemTypes.Includes(Player->GetIdentifiedItemTypes()));
    const TArray<int32> Before = Player->GetCarriedItems();
    TestFalse(TEXT("An existing character cannot replay creation"), Player->ChooseWadClass(Player->GetCharacterClass()));
    TestTrue(TEXT("Rejected repeated creation grants no objects"), Before == Player->GetCarriedItems());
    TestEqual(TEXT("Rejected repeated creation grants no extra gold"), Travel->Quest.GetWalletCopper(), int64(400));

    // Use the saved original stock/prices with the actual Town transaction entry point.
    auto* TownData = static_cast<UNativeWadTownData*>(LoadObject<UObject>(nullptr,
        TEXT("/Game/Maps/WadTown_Assets/Town/TownData.TownData")));
    UClass* ContentClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadTownContent"));
    auto* Content = ContentClass ? static_cast<ANativeWadTownContent*>(World->SpawnActor<AActor>(ContentClass)) : nullptr;
    auto* Catalog = static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr, UNativeWadHudData::ObjectPath));
    if (!TestNotNull(TEXT("Saved source Town stock"), TownData)
        || !TestNotNull(TEXT("Town content actor for actual transactions"), Content)
        || !TestNotNull(TEXT("Saved source item prices"), Catalog)) { return false; }
    Content->Data = TownData;
    constexpr int32 PurchaseType = 426; // BANKUI.C's actual base-tier Food Conservation ring.
    const int32* Tier = TownData->BankStock.Find(PurchaseType);
    const FWadInventoryDetails* Ring = Catalog->ItemDetails.Find(PurchaseType);
    if (!TestNotNull(TEXT("Original Bank offers the tested ring"), Tier)
        || !TestNotNull(TEXT("Source ring has imported rules"), Ring)) { return false; }
    if (!TestTrue(TEXT("The test purchase has a nonzero source price"), Ring->ValueCopper > 0)) { return false; }
    Travel->Quest.ShopTier = *Tier;
    Travel->Quest.SetWalletCopper(0);
    Player->InitializeItemIdentification(false);
    Player->GetInventoryItemDescription(PurchaseType, true);
    TestFalse(TEXT("A merchant description preview never teaches the type"), Player->HasIdentifiedItemType(PurchaseType));
    TestFalse(TEXT("An unaffordable actual purchase is refused"), Player->TradeWadTownItem(PurchaseType, false, true));
    TestFalse(TEXT("A refused purchase does not identify the item"), Player->HasIdentifiedItemType(PurchaseType));
    Travel->Quest.SetWalletCopper(Ring->ValueCopper);
    if (!TestTrue(TEXT("An affordable actual purchase succeeds"), Player->TradeWadTownItem(PurchaseType, false, true))) { return false; }
    TestTrue(TEXT("A successful source purchase identifies the exact type"), Player->HasIdentifiedItemType(PurchaseType));
    TestEqual(TEXT("The successful purchase charges the source price once"), Travel->Quest.GetWalletCopper(), int64(0));
    TestTrue(TEXT("The purchased object can be sold through the actual Bank transaction"),
        Player->TradeWadTownItem(Player->GetCarriedItems().Num() - 1, true, true));
    TestTrue(TEXT("Selling the object does not erase discovery"), Player->HasIdentifiedItemType(PurchaseType));
    return true;
}
