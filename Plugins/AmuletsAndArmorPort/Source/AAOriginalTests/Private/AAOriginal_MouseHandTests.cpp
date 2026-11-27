#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadInventoryView.h"

namespace
{
    struct FMouseHandFixture
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;
        explicit FMouseHandFixture(const TCHAR* Options = nullptr)
        {
            if (!World) { return; }
            World->SetGameInstance(NewObject<UGameInstance>());
            if (Options)
            {
                World->URL = FURL(nullptr, Options, TRAVEL_Absolute);
            }
            Player = World->SpawnActor<ANativeWadPlayer>();
            if (Player)
            {
                Player->DispatchBeginPlay();
                Input = NewObject<UInputComponent>(Player);
                Player->SetupPlayerInputComponent(Input);
            }
        }
        ~FMouseHandFixture() { if (World) { World->DestroyWorld(false); } }
        void Press(FKey Key)
        {
            if (!Input) { return; }
            for (const FInputKeyBinding& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return; }
            }
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalRetainedMouseHand, "AAOriginal.WAD.Gameplay.RetainedMouseHand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalRetainedMouseHand::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FMouseHandFixture Fixture;
    auto* Player = Fixture.Player;
    if (!TestNotNull(TEXT("Character for actual retained locations"), Player)) { return false; }
    Player->InitializeItemIdentification(false);
    Player->CollectItem(803);
    Player->CollectItem(803);
    Player->CollectItem(201);
    auto* Catalog = static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr, UNativeWadHudData::ObjectPath));
    if (!TestNotNull(TEXT("Original item catalog for display stacks"), Catalog)) { return false; }
    const int32 Objects = Player->GetCarriedItems().Num();
    TestTrue(TEXT("Taking one member of a source stack succeeds"), Player->TakeInventoryItem(0));
    NativeWadInventoryView::FView Bag(*Player, *Catalog, true, Player->GetMouseHandSlot());
    TestEqual(TEXT("Held object is excluded from its displayed bag stack"), Bag.Cells[Bag.CellForSlot(1)].Count, 1);
    TestFalse(TEXT("An occupied hand cannot take another object"), Player->TakeInventoryItem(2));
    Player->ClickInventorySlot(2);
    TestEqual(TEXT("Failed take preserves the original object"), Player->GetMouseHandItem(), 803);
    Fixture.Press(EKeys::I);
    Fixture.Press(EKeys::I);
    TestEqual(TEXT("Closing a banner preserves the actual mouse-hand location"), Player->GetMouseHandSlot(), 0);
    Player->ClickInventorySlot(1);
    TestEqual(TEXT("Same-type placement with room rejoins the existing source stack"), Player->GetMouseHandSlot(), INDEX_NONE);
    NativeWadInventoryView::FView Rejoined(*Player, *Catalog, true, Player->GetMouseHandSlot());
    TestEqual(TEXT("Explicit bag return regroups both original objects"), Rejoined.Cells[Rejoined.CellForSlot(0)].Count, 2);
    TestEqual(TEXT("Taking and returning a stack never creates or destroys objects"), Player->GetCarriedItems().Num(), Objects);

    Player->TakeInventoryItem(2);
    TestTrue(TEXT("Empty ready hand accepts the original mace"), Player->SwapMouseAndReady());
    Player->TakeInventoryItem(0);
    TestTrue(TEXT("Mouse and occupied ready locations really swap"), Player->SwapMouseAndReady());
    TestTrue(TEXT("The displaced weapon remains retained after the banner is closed"),
        Player->GetMouseHandItem() == 201 && Player->GetReadyItem() == 803 && !Player->IsInventoryOpen());
    Player->ConsumeItem(803); // Earlier removal shifts both distinct location pointers.
    TestTrue(TEXT("Removing an earlier ready item clears ready and remaps held index"),
        Player->GetReadySlot() == INDEX_NONE && Player->GetMouseHandSlot() == 1 && Player->GetMouseHandItem() == 201);
    Player->IdentifyEquippedItems();
    TestTrue(TEXT("Identify Readied includes retained mouse hand with banners closed"), Player->HasIdentifiedItemType(201));
    Player->StoreMouseHandInInventory();
    Player->CollectItem(417);
    Player->TakeInventoryItem(Player->GetCarriedItems().Num() - 1);
    TestEqual(TEXT("An item merely held has no worn shielding"), Player->GetStatusPower(SpellShieldStatus), 0);
    Player->ClickJewelrySlot(Ring1Slot);
    TestEqual(TEXT("Placing the actual held ring applies one owner"), Player->GetStatusPower(SpellShieldStatus), 15);
    Player->CollectItem(415);
    Player->TakeInventoryItem(Player->GetCarriedItems().Num() - 1);
    Player->ClickJewelrySlot(Ring1Slot);
    TestTrue(TEXT("Replacing jewelry retains the displaced object and only the new worn owner"),
        Player->GetMouseHandItem() == 417 && Player->GetStatusPower(SpellShieldStatus) == 5);
    Player->StoreMouseHandInInventory();
    Player->ClickJewelrySlot(Ring1Slot);
    TestTrue(TEXT("Removing jewelry moves it into the hand and stops its passive"),
        Player->GetMouseHandItem() == 415 && Player->GetStatusPower(SpellShieldStatus) == 0);
    Player->CollectAmmo(0, 12);
    Player->RetrieveAmmo(0);
    TestEqual(TEXT("Ammo withdrawal refuses an occupied hand without spending bolts"), Player->GetAmmoReserve(0), 12);
    Player->ConsumeItem(415);
    TestEqual(TEXT("Destroying the held object clears its location"), Player->GetMouseHandSlot(), INDEX_NONE);
    Player->RetrieveAmmo(0);
    TestTrue(TEXT("Ammo withdrawal places exactly one quiver into the shared hand"),
        Player->GetMouseHandItem() == CrossbowAmmo[0].Quiver && Player->GetAmmoReserve(0) == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMouseHandRestore, "AAOriginal.WAD.Gameplay.MouseHandRestore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalMouseHandRestore::RunTest(const FString& Parameters)
{
    const FString Base = TEXT("/Game/Maps/L30?WadCreated=1?WadRuneRules=1?WadItems=803,201,417?WadReady=1?WadJewelry=-1,2,-1,-1,-1");
    for (const int32 Held : {0, 1, 2, -1, 99})
    {
        FMouseHandFixture Fixture(*(Base + FString::Printf(TEXT("?WadMouseHand=%d"), Held)));
        if (!Fixture.Player) { return false; }
        TestEqual(TEXT("Travel validates distinct held location instead of aliasing equipped items or clamping invalid slots"),
            Fixture.Player->GetMouseHandSlot(), Held == 0 ? 0 : INDEX_NONE);
    }
    FMouseHandFixture Legacy(*Base);
    if (!Legacy.Player) { return false; }
    TestEqual(TEXT("An older URL/checkpoint without a mouse-hand field starts empty"), Legacy.Player->GetMouseHandSlot(), INDEX_NONE);
    for (const TCHAR* Malformed : {TEXT("0junk"), TEXT("4294967296"), TEXT("0.0"), TEXT("+0")})
    {
        FMouseHandFixture Invalid(*(Base + TEXT("?WadMouseHand=") + Malformed));
        if (!Invalid.Player) { return false; }
        TestEqual(TEXT("Malformed or overflowing held-slot tokens cannot alias a valid object"), Invalid.Player->GetMouseHandSlot(), INDEX_NONE);
    }

    FMouseHandFixture Knight(TEXT("/Game/Maps/L30?WadClass=1?WadCreated=1?WadRuneRules=1?WadItems=37569,37573?WadReady=-1"));
    auto* Player = Knight.Player;
    if (!Player) { return false; }
    Player->TakeInventoryItem(0);
    Player->ClickArmorSlot(NativeWadRules::ChestSlot);
    TestTrue(TEXT("Source armor is worn through its equipment target"), Player->IsWornInventorySlot(0));
    Player->TakeInventoryItem(1);
    Player->ClickArmorSlot(NativeWadRules::ChestSlot);
    TestTrue(TEXT("Armor replacement swaps the old exact object to the mouse hand"),
        Player->GetMouseHandSlot() == 0 && Player->IsWornInventorySlot(1) && !Player->IsWornInventorySlot(0));
    TestEqual(TEXT("Armor replacement has one electrical protection owner"), Player->GetStatusPower(NativeWadRules::ElectricityResistStatus), 1);
    Player->StoreMouseHandInInventory();
    Player->ClickArmorSlot(NativeWadRules::ChestSlot);
    TestTrue(TEXT("Unequipping armor retains its object and removes protection"),
        Player->GetMouseHandSlot() == 1 && Player->GetStatusPower(NativeWadRules::ElectricityResistStatus) == 0);

    FMouseHandFixture Priest(TEXT("/Game/Maps/L30?WadCreated=1?WadRuneRules=1?WadItems=201,37573?WadReady=0?WadMouseHand=1"));
    if (!Priest.Player) { return false; }
    TestFalse(TEXT("Source Priest cannot equip the tested plate"), Priest.Player->CanUseWadItem(37573));
    Priest.Player->ClickArmorSlot(NativeWadRules::ChestSlot);
    TestTrue(TEXT("Class refusal preserves both held and ready locations"),
        Priest.Player->GetMouseHandSlot() == 1 && Priest.Player->GetReadySlot() == 0 && !Priest.Player->IsWornInventorySlot(1));
    TestFalse(TEXT("Ready class refusal is also non-destructive"), Priest.Player->SwapMouseAndReady());
    TestTrue(TEXT("Rejected ready swap leaves the two original objects unchanged"),
        Priest.Player->GetMouseHandSlot() == 1 && Priest.Player->GetReadySlot() == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalMouseHandBagPage, "AAOriginal.WAD.Gameplay.MouseHandBagPage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalMouseHandBagPage::RunTest(const FString& Parameters)
{
    auto* Catalog = static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr, UNativeWadHudData::ObjectPath));
    if (!TestNotNull(TEXT("Original inventory art/rules"), Catalog)) { return false; }
    for (int32 FullCells : {48, 96})
    {
        FMouseHandFixture Fixture;
        if (!Fixture.Player) { return false; }
        auto& Player = *Fixture.Player;
        for (int32 Item = 0; Item <= FullCells; ++Item) { Player.CollectItem(NativeWadRules::IronDagger); }
        if (!TestTrue(TEXT("Pick up the extra distinct object while the bag pages are full"), Player.TakeInventoryItem(FullCells))) { return false; }
        const NativeWadInventoryView::FView Bag(Player, *Catalog, true, Player.GetMouseHandSlot());
        TestEqual(TEXT("Located objects are not counted as phantom bag entries"), Bag.Cells.Num(), FullCells);
        TestEqual(TEXT("A held object makes an actual extra placement page available"), Bag.PageCount(), FullCells / NativeWadInventoryView::SlotsPerPage + 1);
        const int32 FirstTarget = (Bag.PageCount() - 1) * NativeWadInventoryView::SlotsPerPage;
        TestFalse(TEXT("The final page's first cell is a real empty drop target"), Bag.Cells.IsValidIndex(FirstTarget));
        TestTrue(TEXT("The empty target lies inside the source bag hit rectangle"),
            NativeWadInventoryView::Bag.IsInside(NativeWadInventoryView::SlotBox(FirstTarget).GetCenter()));
        TestEqual(TEXT("Making placement space does not return or copy the held object behind the player's back"), Player.GetMouseHandSlot(), FullCells);
        TestEqual(TEXT("Exactly the original number of owned objects remains"), Player.GetCarriedItems().Num(), FullCells + 1);
    }
    return true;
}
