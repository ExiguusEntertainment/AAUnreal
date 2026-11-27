#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadFlightOwners.h"

namespace
{
    struct FFlightOwnerFixture
    {
        UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;
        explicit FFlightOwnerFixture(const TCHAR* Saved = nullptr)
        {
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            const FString Options = Saved ? FString(Saved) : FString(TEXT("/Game/Maps/L30?WadClass=2?WadCreated=1?WadRuneRules=1")
                TEXT("?WadLevel=100?WadAttributes=100,100,100,100,100,100?WadMaxHealth=1700?WadHealth=1000")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadReady=-1")
                TEXT("?WadItems=300,301,302,303,304,305,306,307,308"));
            World->URL = FURL(nullptr, *Options, TRAVEL_Absolute);
            Player = World->SpawnActor<ANativeWadPlayer>();
            auto* Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }
        ~FFlightOwnerFixture()
        {
            Instance->Shutdown();
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Valid() const { return Player && Input; }
        bool Press(FKey Key) const
        {
            for (const auto& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }
        bool Cast(const TCHAR* Runes) const
        {
            const FKey Keys[] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
                EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
            const int32 Previous = Player->GetRunes().Len();
            for (int32 Index = 0; Index < Previous; ++Index) { if (!Press(EKeys::BackSpace)) { return false; } }
            for (const TCHAR* Rune = Runes; *Rune; ++Rune)
            { if (*Rune < TEXT('1') || *Rune > TEXT('9') || !Press(Keys[*Rune - TEXT('1')])) { return false; } }
            return Press(EKeys::RightMouseButton);
        }
        int32 Add(int32 Type) const
        { return Player->CollectItem(Type) ? Player->GetCarriedItems().Num() - 1 : INDEX_NONE; }
        bool Ready(int32 Slot) const
        {
            Player->StoreMouseHandInInventory();
            if (!Player->TakeInventoryItem(Slot) || !Player->SwapMouseAndReady()) { return false; }
            Player->StoreMouseHandInInventory();
            return Player->GetReadySlot() == Slot;
        }
        bool Wear(int32 Slot) const
        {
            Player->StoreMouseHandInInventory();
            if (!Player->TakeInventoryItem(Slot)) { return false; }
            Player->ClickJewelrySlot(NativeWadRules::NeckSlot);
            Player->StoreMouseHandInInventory();
            return Player->GetJewelryInventorySlot(NativeWadRules::NeckSlot) == Slot;
        }
        void Earthbind() const
        { Player->ReceiveTypedHit(1, NativeWadRules::SpecialDamageFlag(NativeWadRules::ESpecialDamage::Earthbind)); }
        void Dispel() const
        { Player->ReceiveTypedHit(1, NativeWadRules::SpecialDamageFlag(NativeWadRules::ESpecialDamage::Dispel)); }
        void Unready() const
        { Player->StoreMouseHandInInventory(); Player->SwapMouseAndReady(); Player->StoreMouseHandInInventory(); }
        FString Save() const
        {
            const FURL Town(nullptr, TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"), TRAVEL_Absolute);
            if (!World->GetAuthGameMode() && !World->SetGameMode(Town)) { return {}; }
            const FString Slot = TEXT("NativeFlightOwnerTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot, 0); };
            if (!Player->SaveWadCheckpoint(Slot)) { return {}; }
            const auto* Saved = static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
            return Saved ? Saved->LevelPackage + TEXT("?") + Saved->PlayerOptions : FString();
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalFlightEquipmentOwners, "AAOriginal.WAD.Gameplay.FlightEquipmentOwners",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalFlightEquipmentOwners::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FFlightOwnerFixture Fixture;
    if (!Fixture.Valid()) { return false; }
    auto& Player = *Fixture.Player;
    const int32 A = Fixture.Add(FlyingAmulet), B = Fixture.Add(FlyingAmulet), Weapon = Fixture.Add(IronDagger);
    if (!TestTrue(TEXT("Wear original flight amulet A through the equipment location"), Fixture.Wear(A))) { return false; }
    TestTrue(TEXT("A's READY grants flight"), Player.CanFly());
    Fixture.Earthbind();
    TestFalse(TEXT("Earthbind removes A's effect without consuming the worn object"), Player.CanFly());
    if (!TestTrue(TEXT("A new separate amulet B fits the source ready hand"), Fixture.Ready(B))) { return false; }
    TestTrue(TEXT("Only B's fresh READY restores flight"), Player.CanFly());
    Fixture.Unready();
    TestFalse(TEXT("Removing B cannot resurrect the older Earthbound A owner"), Player.CanFly());
    if (!Fixture.Ready(Weapon)) { return false; }
    TestFalse(TEXT("Readying an unrelated weapon cannot restart A"), Player.CanFly());
    Player.ClickJewelrySlot(NeckSlot);
    TestEqual(TEXT("UNREADY transfers A to the retained mouse hand"), Player.GetMouseHandSlot(), A);
    Player.ClickJewelrySlot(NeckSlot);
    TestTrue(TEXT("A's own remove/re-equip sends a new READY and restores its effect"), Player.CanFly());
    if (!Fixture.Ready(B)) { return false; }
    Fixture.Earthbind();
    TestFalse(TEXT("Earthbind suppresses both simultaneous equipped owners"), Player.CanFly());
    Fixture.Dispel();
    TestTrue(TEXT("Source Dispel re-READY restores equipped flight even without temporary effects"), Player.CanFly());
    Fixture.Unready();
    TestTrue(TEXT("Restored worn A continues after restored B is removed"), Player.CanFly());
    TestEqual(TEXT("Owner lifecycle neither consumes nor duplicates amulets"), Player.GetItemCount(FlyingAmulet), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalFlightOwnerPersistence, "AAOriginal.WAD.Gameplay.FlightOwnerPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalFlightOwnerPersistence::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FFlightOwnerFixture Fixture;
    if (!Fixture.Valid()) { return false; }
    Fixture.Add(Berry);
    const int32 A = Fixture.Add(FlyingAmulet), B = Fixture.Add(FlyingAmulet);
    if (!Fixture.Wear(A)) { return false; }
    Fixture.Earthbind();
    if (!Fixture.Ready(B)) { return false; }
    if (!TestTrue(TEXT("Consuming an earlier inventory owner remaps later equipment and suppression together"), Fixture.Player->ConsumeItem(Berry))) { return false; }
    TestEqual(TEXT("Worn amulet index shifts with the carried array"), Fixture.Player->GetJewelryInventorySlot(NeckSlot), A - 1);
    const FString Options = Fixture.Save();
    if (!TestFalse(TEXT("Actual disk writer saved the partial suppression state"), Options.IsEmpty())) { return false; }
    const FURL URL(nullptr, *Options, TRAVEL_Absolute);
    TestEqual(TEXT("Only A is recorded in the canonical individual-owner payload"), FString(URL.GetOption(TEXT("WadFlightOwners="), TEXT("missing"))),
        TEXT("1~") + LexToString(A - 1));
    {
        FFlightOwnerFixture Restored(*Options);
        if (!Restored.Valid()) { return false; }
        TestTrue(TEXT("Fresh actor restores active B alongside suppressed A"), Restored.Player->CanFly());
        Restored.Unready();
        TestFalse(TEXT("After disk/new-player restoration, removing B still cannot resurrect A"), Restored.Player->CanFly());
        Restored.Dispel();
        TestTrue(TEXT("Dispel re-READY works after restart too"), Restored.Player->CanFly());
        const FString ActiveOptions = Restored.Save();
        if (ActiveOptions.IsEmpty()) { return false; }
        FURL Modern(nullptr, *ActiveOptions, TRAVEL_Absolute);
        Modern.AddOption(TEXT("WadFlightSuppressed=1"));
        FFlightOwnerFixture EmptyOverrides(*Modern.ToString());
        if (!EmptyOverrides.Valid()) { return false; }
        TestTrue(TEXT("Explicit empty modern owners override stale legacy global suppression"), EmptyOverrides.Player->CanFly());
    }
    {
        FURL Legacy(nullptr, *Options, TRAVEL_Absolute);
        Legacy.RemoveOption(TEXT("WadFlightOwners"));
        Legacy.AddOption(TEXT("WadFlightSuppressed=1"));
        FFlightOwnerFixture Migrated(*Legacy.ToString());
        if (!Migrated.Valid()) { return false; }
        TestFalse(TEXT("Legacy global restriction migrates conservatively to all equipped owners without guessed history"), Migrated.Player->CanFly());
        Migrated.Unready();
        TestFalse(TEXT("Legacy migrated A remains Earthbound after B is removed"), Migrated.Player->CanFly());
        Migrated.Dispel();
        TestTrue(TEXT("A source re-READY restores migrated equipment normally"), Migrated.Player->CanFly());
    }
    {
        Fixture.Earthbind();
        const FString BothOptions = Fixture.Save();
        if (BothOptions.IsEmpty()) { return false; }
        FFlightOwnerFixture Both(*BothOptions);
        if (!Both.Valid()) { return false; }
        TestFalse(TEXT("Both suppressed equipment identities survive disk and a fresh actor"), Both.Player->CanFly());
        if (!TestTrue(TEXT("Removing suppressed A deletes only its owner and shifts B"), Both.Player->ConsumeItem(FlyingAmulet))) { return false; }
        TestFalse(TEXT("Removing A cannot reactivate still-suppressed B after its index shifts"), Both.Player->CanFly());
        Both.Unready();
        if (!Both.Ready(Both.Player->GetCarriedItems().Find(FlyingAmulet))) { return false; }
        TestTrue(TEXT("B's own new READY works after owner deletion/remapping"), Both.Player->CanFly());
    }
    {
        FURL Invalid(nullptr, *Options, TRAVEL_Absolute);
        Invalid.AddOption(TEXT("WadFlightOwners=1~2147483648"));
        FFlightOwnerFixture Refused(*Invalid.ToString());
        if (!Refused.Valid()) { return false; }
        TestFalse(TEXT("Invalid modern owner data does not silently reactivate equipped flight"), Refused.Player->CanFly());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalFlightTemporaryOwners, "AAOriginal.WAD.Gameplay.FlightTemporaryOwners",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalFlightTemporaryOwners::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FFlightOwnerFixture Fixture;
    if (!Fixture.Valid()) { return false; }
    auto& Player = *Fixture.Player;
    const int32 A = Fixture.Add(FlyingAmulet);
    if (!Fixture.Wear(A)) { return false; }
    Fixture.Earthbind();
    if (!Fixture.Cast(TEXT("167"))) { return false; }
    TestTrue(TEXT("A fresh timed spell can fly while the equipped amulet remains suppressed"), Player.CanFly());
    TestTrue(TEXT("The exact spell41 temporary owner remains independently tracked"), Player.GetOwnedEffectRemaining(TEXT("spell:41")) > 0.0);
    Player.Tick(30.1f);
    TestFalse(TEXT("Expired timed flight cannot reactivate the suppressed amulet"), Player.CanFly());
    const int32 Potion = Fixture.Add(FlyingPotion);
    if (!Fixture.Ready(Potion) || !Fixture.Press(EKeys::LeftControl)) { return false; }
    TestTrue(TEXT("Ordinary potion USE adds its own independent temporary flight"), Player.CanFly());
    TestTrue(TEXT("Potion805 retains its source consumable owner"), Player.GetOwnedEffectRemaining(TEXT("item:805")) > 0.0);
    Fixture.Earthbind();
    TestFalse(TEXT("Earthbind removes timed flight as well as suppressing currently equipped owners"), Player.CanFly());
    TestEqual(TEXT("Earthbind removed the potion owner rather than pausing its timer"), Player.GetOwnedEffectRemaining(TEXT("item:805")), 0.0);
    if (!Fixture.Cast(TEXT("924"))) { return false; }
    const int32 Orb = Fixture.Add(OrbOfDefense);
    const int32 Health = Player.GetHealth();
    if (!Fixture.Ready(Orb)) { return false; }
    TestEqual(TEXT("The orb's ordinary READY still pays raw300 before its own protection"), Player.GetHealth(), Health - 300);
    Fixture.Dispel();
    TestEqual(TEXT("Dispel removes the sole temporary Ward owner before re-READY orb cost"), Player.GetOwnedEffectCount(), 0);
    TestEqual(TEXT("Flight restoration preserves the existing source orb re-READY cost"), Player.GetHealth(), Health - 600);
    TestFalse(TEXT("Neither source orb cost caused true defeat at this fixture health"), Player.IsDefeated());
    TestTrue(TEXT("Dispel restores suppressed worn flight alongside the orb"), Player.CanFly());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalFlightOwnerCodec, "AAOriginal.WAD.Gameplay.FlightOwnerCodec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalFlightOwnerCodec::RunTest(const FString& Parameters)
{
    TArray<int32> Owners;
    TestTrue(TEXT("Modern explicit empty payload is valid"), NativeWadFlightOwners::Decode(TEXT("1"), Owners));
    TestTrue(TEXT("Canonical distinct source object indices decode"), NativeWadFlightOwners::Decode(TEXT("1~0,9,21"), Owners));
    TestEqual(TEXT("All distinct owner indices survive"), Owners.Num(), 3);
    for (const TCHAR* Bad : {TEXT(""), TEXT("0"), TEXT("1~"), TEXT("1~1,"), TEXT("1~1,1"), TEXT("1~2,1"),
        TEXT("1~01"), TEXT("1~-1"), TEXT("1~+1"), TEXT("1~ 1"), TEXT("1~1junk"), TEXT("1~2147483648"), TEXT("1~42949672960")})
    { TestFalse(TEXT("Malformed or overflowing owner data is refused"), NativeWadFlightOwners::Decode(Bad, Owners)); }
    return true;
}
