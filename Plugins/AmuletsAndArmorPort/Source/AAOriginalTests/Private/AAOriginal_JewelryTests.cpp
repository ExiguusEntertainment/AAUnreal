#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadPlayer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWornJewelry, "AAOriginal.WAD.Gameplay.WornJewelry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalWornJewelry::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for original jewelry locations"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->SetGameInstance(NewObject<UGameInstance>());
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Player || !Controller) { return false; }
    World->AddController(Controller);
    Controller->Possess(Player);
    UInputComponent* Input = NewObject<UInputComponent>(Player);
    Player->SetupPlayerInputComponent(Input);
    const auto Press = [&](const TCHAR* Name)
    {
        for (const FInputKeyBinding& Binding : Input->KeyBindings)
        {
            if (Binding.KeyEvent == IE_Pressed && Binding.Chord.Key.GetFName() == FName(Name))
            {
                Binding.KeyDelegate.Execute(Binding.Chord.Key);
                return true;
            }
        }
        return false;
    };
    const auto SelectForReady = [&](int32 Slot)
    {
        if (!Press(TEXT("I"))) { return false; }
        Player->ClickInventorySlot(Slot);
        Player->ClickInventorySlot(INDEX_NONE);
        return Press(TEXT("I"));
    };
    const auto AddAndReady = [&](int32 Type)
    {
        Player->CollectItem(Type);
        return SelectForReady(Player->GetCarriedItems().Num() - 1);
    };
    const auto Wear = [&](int32 Slot, int32 Location)
    {
        if (!Press(TEXT("I"))) { return false; }
        Player->ClickInventorySlot(Slot);
        Player->ClickJewelrySlot(Location);
        return Press(TEXT("I"));
    };
    const auto Remove = [&](int32 Location)
    {
        if (!Press(TEXT("I"))) { return false; }
        Player->ClickJewelrySlot(Location);
        return Press(TEXT("I"));
    };
    const auto AddAndWear = [&](int32 Type, int32 Location)
    {
        Player->CollectItem(Type);
        return Wear(Player->GetCarriedItems().Num() - 1, Location);
    };
    Player->CollectItem(Berry); // Removing this earlier slot must shift every saved equipment index.
    if (!TestTrue(TEXT("Ready an original class-permitted iron mace"), AddAndReady(201))) { return false; }
    TestEqual(TEXT("The mace is a functional ready-hand weapon"), Player->GetEquippedWeapon(), 201);
    for (int32 Location = Ring1Slot; Location <= Ring4Slot; ++Location)
    {
        if (!AddAndWear(408, Location)) { return false; }
        const int32 Slot = Player->GetJewelryInventorySlot(Location);
        TestTrue(TEXT("The selected original ring location holds the object"),
            Player->GetCarriedItems().IsValidIndex(Slot) && Player->GetCarriedItems()[Slot] == 408);
        TestTrue(TEXT("A ring is marked as worn"), Player->IsWornInventorySlot(Slot));
    }
    TestEqual(TEXT("Four distinct source ring owners add their ten-point strengths"), Player->GetStatusPower(StrengthStatus), 40);
    TestEqual(TEXT("Wearing rings leaves the weapon ready"), Player->GetEquippedWeapon(), 201);
    const int32 FirstRing = Player->GetJewelryInventorySlot(Ring1Slot);
    Player->CollectItem(407);
    Press(TEXT("I"));
    Player->ClickInventorySlot(Player->GetCarriedItems().Num() - 1);
    Player->ClickJewelrySlot(Ring1Slot);
    TestEqual(TEXT("A fifth ring swaps with an occupied location instead of adding a fifth worn owner"), Player->GetStatusPower(StrengthStatus), 35);
    TestEqual(TEXT("The displaced original ring is held by the mouse"), Player->GetSelectedInventorySlot(), FirstRing);
    Press(TEXT("I"));
    Wear(FirstRing, Ring1Slot);
    TestEqual(TEXT("Restoring that ring restores exactly the four source owners"), Player->GetStatusPower(StrengthStatus), 40);

    AddAndWear(606, NeckSlot);
    TestTrue(TEXT("An equipped amulet protects while the weapon remains ready"),
        (Player->GetResistanceMask() & DamageFlag(EProjectileDamage::Poison)) != 0 && Player->GetEquippedWeapon() == 201);
    AddAndWear(607, NeckSlot);
    TestEqual(TEXT("A second amulet replaces the single necklace location"),
        Player->GetCarriedItems()[Player->GetJewelryInventorySlot(NeckSlot)], 607);
    TestFalse(TEXT("The displaced amulet provides no poison protection"), (Player->GetResistanceMask() & DamageFlag(EProjectileDamage::Poison)) != 0);
    Wear(Player->GetCarriedItems().Find(606), NeckSlot);

    const int32 PreviousNeck = Player->GetJewelryInventorySlot(NeckSlot);
    const int32 PreviousReady = Player->GetReadySlot();
    Player->ConsumeItem(Berry);
    TestEqual(TEXT("Removing an earlier inventory object shifts the necklace index"), Player->GetJewelryInventorySlot(NeckSlot), PreviousNeck - 1);
    TestEqual(TEXT("The same removal shifts the ready hand"), Player->GetReadySlot(), PreviousReady - 1);
    TestEqual(TEXT("Index maintenance does not duplicate ring effects"), Player->GetStatusPower(StrengthStatus), 40);

    Remove(Ring1Slot);
    TestEqual(TEXT("Selecting a worn ring removes only that owner"), Player->GetStatusPower(StrengthStatus), 30);
    Wear(Player->GetCarriedItems().Find(407), Ring1Slot);
    TestEqual(TEXT("A free location accepts the other ring"), Player->GetStatusPower(StrengthStatus), 35);
    Player->ConsumeItem(407);
    TestEqual(TEXT("Removing a worn object clears its location"), Player->GetJewelryInventorySlot(Ring1Slot), INDEX_NONE);
    TestEqual(TEXT("Removing that object removes exactly its own effect"), Player->GetStatusPower(StrengthStatus), 30);

    AddAndWear(415, Ring1Slot);
    TestEqual(TEXT("The original shielding ring supplies its authored power while worn"), Player->GetArmorRating(), 5);
    Player->ConsumeItem(415);
    TestEqual(TEXT("Removing shielding jewelry restores the prior armor"), Player->GetArmorRating(), 0);
    AddAndWear(WaterWalkRing, Ring1Slot);
    TestTrue(TEXT("Water walking remains equipped alongside the weapon"), Player->HasEquippedItem(WaterWalkRing) && Player->GetEquippedWeapon() == 201);
    Player->ConsumeItem(WaterWalkRing);

    Remove(NeckSlot);
    AddAndWear(FlyingAmulet, NeckSlot);
    TestTrue(TEXT("A worn flight amulet works without occupying the weapon hand"), Player->CanFly() && Player->GetEquippedWeapon() == 201);
    Player->ReceiveTypedHit(5000, SpecialDamageFlag(ESpecialDamage::Earthbind));
    TestFalse(TEXT("Earthbind suppresses worn amulet flight"), Player->CanFly());
    AddAndReady(Berry);
    Player->Tick(0.1f);
    TestFalse(TEXT("Changing the ready hand does not restart a suppressed worn flight owner"), Player->CanFly());
    SelectForReady(Player->GetCarriedItems().Find(201));
    Player->Tick(0.1f);
    TestFalse(TEXT("Returning to the weapon still leaves worn flight suppressed"), Player->CanFly());
    Remove(NeckSlot);
    Wear(Player->GetCarriedItems().Find(FlyingAmulet), NeckSlot);
    TestTrue(TEXT("Removing and equipping the amulet creates a new READY trigger"), Player->CanFly());
    Player->ConsumeItem(FlyingAmulet);
    TestFalse(TEXT("Removing the worn flight item ends its effect"), Player->CanFly());
    AddAndWear(FeatherfallAmulet, NeckSlot);
    TestTrue(TEXT("The worn original Falling amulet protects with a weapon ready"), Player->HasFeatherfall() && Player->GetEquippedWeapon() == 201);
    Player->ConsumeItem(FeatherfallAmulet);
    TestFalse(TEXT("Removing the Falling amulet removes its protection"), Player->HasFeatherfall());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSourceAttributeRules, "AAOriginal.WAD.Gameplay.SourceAttributeRules",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSourceAttributeRules::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    TestEqual(TEXT("The source lower effective-attribute bound is ten"), EffectiveAttribute(15, -20), 10);
    TestEqual(TEXT("The source upper effective-attribute bound is 120"), EffectiveAttribute(100, 25), 120);
    TestEqual(TEXT("Priest mace base damage precedes random variation"), MeleeBaseDamage(15, 10, PriestClass), 75);
    TestEqual(TEXT("The source Knight class bonus rounds down before the hit roll"), MeleeBaseDamage(15, 10, KnightClass), 112);
    TestEqual(TEXT("The source Sailor class bonus is one quarter"), MeleeBaseDamage(15, 10, SailorClass), 93);
    TestEqual(TEXT("The source Mercenary class bonus is one quarter"), MeleeBaseDamage(15, 10, MercenaryClass), 93);
    TestEqual(TEXT("The commented-out Warlock bonus remains disabled"), MeleeBaseDamage(15, 10, WarlockClass), 75);
    TestEqual(TEXT("The commented-out Paladin bonus remains disabled"), MeleeBaseDamage(15, 10, PaladinClass), 75);
    TestEqual(TEXT("A noncritical minimum hit retains base damage"), ResolveMeleeRoll(75, 25, 0, 25).Damage, 75);
    TestEqual(TEXT("The final noncritical variation is twice base minus one"), ResolveMeleeRoll(75, 25, 74, 25).Damage, 149);
    TestTrue(TEXT("The last successful accuracy roll is critical"), ResolveMeleeRoll(75, 25, 0, 24).bCritical);
    TestFalse(TEXT("The first unsuccessful accuracy roll is not critical"), ResolveMeleeRoll(75, 25, 0, 25).bCritical);
    TestEqual(TEXT("Critical doubling follows damage variation"), ResolveMeleeRoll(75, 120, 74, 119).Damage, 298);
    TestFalse(TEXT("Even accuracy 120 leaves unsuccessful rolls"), ResolveMeleeRoll(75, 120, 74, 120).bCritical);
    TestEqual(TEXT("Ordinary stealth has its own concealment chance"), ConcealmentThreshold(40, false, false), 20);
    TestEqual(TEXT("Translucency supplies the source minimum concealment"), ConcealmentThreshold(120, false, true), 100);
    TestEqual(TEXT("Invisibility supplies the source minimum concealment"), ConcealmentThreshold(120, true, true), 190);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalBankPassiveEffects, "AAOriginal.WAD.Gameplay.BankPassiveEffects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalBankPassiveEffects::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for source Bank passive effects"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->SetGameInstance(NewObject<UGameInstance>());
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Player || !Controller) { return false; }
    World->AddController(Controller);
    Controller->Possess(Player);
    UInputComponent* Input = NewObject<UInputComponent>(Player);
    Player->SetupPlayerInputComponent(Input);
    const auto Press = [&](const TCHAR* Name)
    {
        for (const FInputKeyBinding& Binding : Input->KeyBindings)
        {
            if (Binding.KeyEvent == IE_Pressed && Binding.Chord.Key.GetFName() == FName(Name))
            { Binding.KeyDelegate.Execute(Binding.Chord.Key); return; }
        }
        AddError(FString::Printf(TEXT("Missing normal input binding %s"), Name));
    };
    const auto Wear = [&](int32 Type, int32 Location)
    {
        Player->CollectItem(Type);
        Press(TEXT("I"));
        Player->ClickInventorySlot(Player->GetCarriedItems().Num() - 1);
        Player->ClickJewelrySlot(Location);
        Press(TEXT("I"));
    };
    const auto Clear = [&]()
    {
        Press(TEXT("I"));
        for (int32 Location = 0; Location < JewelrySlotCount; ++Location)
        {
            // Closing and reopening puts the selected displaced object back in the bag.
            Player->ClickJewelrySlot(Location);
            Press(TEXT("I"));
            Press(TEXT("I"));
        }
        Press(TEXT("I"));
    };
    TestEqual(TEXT("The source Priest health regeneration base is 150 >> 3"), Player->GetHealthRegeneration(), 18);
    TestEqual(TEXT("The source Priest mana regeneration base is 630 >> 2"), Player->GetManaRegeneration(), 157);
    Wear(600, NeckSlot);
    TestEqual(TEXT("The Bank magic amulet affects effective Magic"), Player->GetEffectiveAttribute(MagicAttribute), 60);
    TestEqual(TEXT("The permanent Magic stat stays unmodified"), Player->GetAttributes()[MagicAttribute], 35);
    TestEqual(TEXT("Magic modifiers do not invent a mana-regeneration bonus"), Player->GetManaRegeneration(), 157);
    Wear(619, NeckSlot);
    Wear(405, Ring1Slot);
    TestEqual(TEXT("A displaced magic amulet stops contributing"), Player->GetEffectiveAttribute(MagicAttribute), 35);
    TestEqual(TEXT("Health regen adds the source ring's 200 before dividing by eight"), Player->GetHealthRegeneration(), 43);
    TestEqual(TEXT("Mana regen adds the source amulet's 500 before dividing by four"), Player->GetManaRegeneration(), 282);
    Player->ReceiveEncounterHit(500);
    Player->RestoreWadVital(10, -500);
    const int32 Health = Player->GetHealth();
    const int32 Mana = Player->GetMana();
    Player->Tick(6.0f);
    TestEqual(TEXT("The actual needs tick applies worn health regeneration"), Player->GetHealth(), Health + 43);
    TestEqual(TEXT("The actual needs tick applies worn mana regeneration"), Player->GetMana(), Mana + 282);
    Clear();
    Player->RestoreWadVital(7, MaxFood);
    Player->RestoreWadVital(8, MaxFood);
    TestEqual(TEXT("Removing the ring restores base health recovery"), Player->GetHealthRegeneration(), 18);
    TestEqual(TEXT("Removing the amulet restores base mana recovery"), Player->GetManaRegeneration(), 157);

    Wear(421, Ring1Slot);
    Wear(438, Ring2Slot);
    Wear(430, Ring3Slot);
    TestEqual(TEXT("The Bank accuracy ring adds its original ten points"), Player->GetEffectiveAttribute(AccuracyAttribute), 25);
    TestEqual(TEXT("The Bank thief ring adds its original twenty stealth points"), Player->GetEffectiveAttribute(StealthAttribute), 40);
    TestEqual(TEXT("The Bank movement ring adds its original ten speed points"), Player->GetEffectiveAttribute(SpeedAttribute), 30);
    TestEqual(TEXT("Jewelry does not change the saved base accuracy"), Player->GetAttributes()[AccuracyAttribute], 15);
    Player->ReceiveTypedHit(5000, SpecialDamageFlag(ESpecialDamage::Slow));
    TestEqual(TEXT("Slow and the worn speed bonus share the source ten-point lower bound"), Player->GetEffectiveAttribute(SpeedAttribute), 10);
    Clear();
    TestEqual(TEXT("Removing accuracy jewelry removes its modifier"), Player->GetEffectiveAttribute(AccuracyAttribute), 15);
    TestEqual(TEXT("Removing stealth jewelry removes its modifier"), Player->GetEffectiveAttribute(StealthAttribute), 20);

    for (int32 Location = Ring1Slot; Location <= Ring4Slot; ++Location) { Wear(411, Location); }
    Wear(604, NeckSlot);
    TestEqual(TEXT("Distinct worn owners retain their individual source powers"), Player->GetStatusPower(StrengthStatus), 125);
    TestEqual(TEXT("Their effective strength respects the source 120 cap"), Player->GetEffectiveAttribute(StrengthAttribute), 120);
    TestEqual(TEXT("The base strength stays fifteen at the cap"), Player->GetAttributes()[StrengthAttribute], 15);
    Clear();
    TestEqual(TEXT("Removing all strength owners restores the unchanged base"), Player->GetEffectiveAttribute(StrengthAttribute), 15);
    Player->ReceiveTrapPoison(50);
    const int32 PoisonBeforeProtection = Player->GetPoisonLevel();
    Wear(606, NeckSlot);
    const int32 ProtectedHealth = Player->GetHealth();
    const int32 ProtectedRecovery = Player->GetHealthRegeneration();
    Player->Tick(6.0f);
    TestEqual(TEXT("Worn poison protection prevents periodic poison injury while normal recovery continues"),
        Player->GetHealth(), FMath::Min(Player->GetMaxHealth(), ProtectedHealth + ProtectedRecovery));
    TestTrue(TEXT("Poison continues to decay under its worn protection"), Player->GetPoisonLevel() < PoisonBeforeProtection);
    return true;
}
