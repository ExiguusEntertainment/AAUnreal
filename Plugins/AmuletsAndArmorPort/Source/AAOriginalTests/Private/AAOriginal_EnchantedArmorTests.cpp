#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalEnchantedArmor, "AAOriginal.WAD.Gameplay.EnchantedArmor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalEnchantedArmor::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for original enchanted armor"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->SetGameInstance(NewObject<UGameInstance>());
    World->URL.AddOption(TEXT("WadClass=1")); // Original Knight can wear the tested plate pieces.
    World->URL.AddOption(TEXT("WadCreated=1"));
    World->URL.AddOption(TEXT("WadRuneRules=1"));
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Player || !Controller) { return false; }
    World->AddController(Controller);
    Controller->Possess(Player);
    Player->DispatchBeginPlay();
    UInputComponent* Input = NewObject<UInputComponent>(Player);
    Player->SetupPlayerInputComponent(Input);
    const auto Press = [&](const TCHAR* Name)
    {
        for (const FInputKeyBinding& Binding : Input->KeyBindings)
        {
            if (Binding.KeyEvent == IE_Pressed && Binding.Chord.Key.GetFName() == FName(Name))
            { Binding.KeyDelegate.Execute(Binding.Chord.Key); return; }
        }
        AddError(FString::Printf(TEXT("Missing normal input %s"), Name));
    };
    const auto Select = [&](int32 Slot)
    {
        Press(TEXT("I"));
        Player->StoreMouseHandInInventory();
        const int32 Type = Player->GetCarriedItems()[Slot];
        const auto* Catalog = static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr, UNativeWadHudData::ObjectPath));
        const auto* Rules = Catalog ? Catalog->ItemDetails.Find(Type) : nullptr;
        if (Rules && Rules->ArmorSlot != INDEX_NONE)
        {
            if (!Player->IsWornInventorySlot(Slot)) { Player->ClickInventorySlot(Slot); }
            Player->ClickArmorSlot(Rules->ArmorSlot);
        }
        else { Player->ClickInventorySlot(Slot); Player->ClickInventorySlot(INDEX_NONE); }
        Press(TEXT("I"));
    };
    const auto Armor = [&](int32 Type)
    {
        Player->CollectItem(Type);
        const int32 Slot = Player->GetCarriedItems().Num() - 1;
        Select(Slot);
        return Slot;
    };
    const auto Jewelry = [&](int32 Type, int32 Location)
    {
        Player->CollectItem(Type);
        Press(TEXT("I"));
        Player->StoreMouseHandInInventory();
        Player->ClickInventorySlot(Player->GetCarriedItems().Num() - 1);
        Player->ClickJewelrySlot(Location);
        Press(TEXT("I"));
    };
    const int32 Chain = Armor(37569);
    if (!TestTrue(TEXT("The actual Obsidian chain tunic is worn"), Player->IsWornInventorySlot(Chain))) { return false; }
    TestEqual(TEXT("Its source electrical protection is one worn object owner"), Player->GetStatusPower(ElectricityResistStatus), 1);
    TestEqual(TEXT("Its source physical armor remains in the weighted average"), Player->GetArmorRating(), 13);
    int32 Health = Player->GetHealth();
    Player->ReceiveTypedHit(400, DamageFlag(EProjectileDamage::Electricity));
    TestEqual(TEXT("Obsidian chain resists the electrical bonus and applies original physical mitigation"), Health - Player->GetHealth(), 75);
    const int32 Plate = Armor(37573);
    TestTrue(TEXT("The class-permitted Obsidian plate replaces the tunic"), Player->IsWornInventorySlot(Plate) && !Player->IsWornInventorySlot(Chain));
    TestEqual(TEXT("Replacing armor does not retain a second phantom effect owner"), Player->GetStatusPower(ElectricityResistStatus), 1);
    Jewelry(617, NeckSlot);
    TestEqual(TEXT("An independently worn protection amulet adds its own owner"), Player->GetStatusPower(ElectricityResistStatus), 2);
    const int32 FirePlate = Armor(41669);
    TestEqual(TEXT("The Pyrinuim breastplate provides its original fire protection"), Player->GetStatusPower(FireResistStatus), 1);
    TestEqual(TEXT("Replacing the Obsidian plate leaves only the independent electrical owner"), Player->GetStatusPower(ElectricityResistStatus), 1);
    Health = Player->GetHealth();
    Player->ReceiveTypedHit(400, DamageFlag(EProjectileDamage::Fire));
    TestEqual(TEXT("Pyrinuim fire protection reaches actual incoming damage"), Health - Player->GetHealth(), 72);
    Select(FirePlate);
    TestEqual(TEXT("Removing enchanted armor removes its own passive"), Player->GetStatusPower(FireResistStatus), 0);
    TestEqual(TEXT("Removing that armor preserves another worn object's passive"), Player->GetStatusPower(ElectricityResistStatus), 1);
    Press(TEXT("I"));
    Player->StoreMouseHandInInventory();
    Player->ClickJewelrySlot(NeckSlot);
    Press(TEXT("I"));
    const int32 AcidPlate = Armor(45765);
    TestEqual(TEXT("Adaminium plate provides its original acid protection"), Player->GetStatusPower(AcidResistStatus), 1);
    Health = Player->GetHealth();
    Player->ReceiveTypedHit(400, DamageFlag(EProjectileDamage::Acid));
    TestEqual(TEXT("Adaminium acid protection reaches actual incoming damage"), Health - Player->GetHealth(), 71);
    Select(AcidPlate);
    TestEqual(TEXT("Its protection ends when the armor is removed"), Player->GetStatusPower(AcidResistStatus), 0);

    Armor(712);
    TestEqual(TEXT("The source shield hand does not grant a global body armor bonus"), Player->GetArmorRating(), 0);
    Jewelry(415, Ring1Slot);
    TestEqual(TEXT("Magical shielding is independent of a physical shield hand"), Player->GetArmorRating(), 5);
    Select(AcidPlate);
    TestEqual(TEXT("Physical body armor stronger than magic is preserved before weighting"), Player->GetArmorRating(), 26);
    Select(AcidPlate);
    Player->CollectItem(808);
    Select(Player->GetCarriedItems().Num() - 1);
    Press(TEXT("LeftControl"));
    TestEqual(TEXT("Potion shielding adds to magical owners before the source cap"), Player->GetArmorRating(), 15);
    for (int32 Location = Ring1Slot; Location <= Ring4Slot; ++Location) { Jewelry(419, Location); }
    Jewelry(615, NeckSlot);
    TestEqual(TEXT("All distinct shield owners retain their authored total"), Player->GetStatusPower(SpellShieldStatus), 130);
    TestEqual(TEXT("The source magical-shield cap is ninety, including active potions"), Player->GetArmorRating(), 90);
    return true;
}
