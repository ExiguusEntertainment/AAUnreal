#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadPlayer.h"

namespace
{
    struct FConservationInputs
    {
        ANativeWadPlayer& Player;
        UInputComponent* Input;

        explicit FConservationInputs(ANativeWadPlayer& InPlayer) : Player(InPlayer), Input(NewObject<UInputComponent>(&InPlayer))
        { Player.SetupPlayerInputComponent(Input); }

        bool Press(const TCHAR* Name) const
        {
            for (const FInputKeyBinding& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord.Key.GetFName() == FName(Name))
                { Binding.KeyDelegate.Execute(Binding.Chord.Key); return true; }
            }
            return false;
        }
        void Wear(int32 Type, int32 Location) const
        {
            Player.CollectItem(Type);
            Press(TEXT("I"));
            Player.ClickInventorySlot(Player.GetCarriedItems().Num() - 1);
            Player.ClickJewelrySlot(Location);
            Press(TEXT("I"));
        }
        void Remove(int32 Location) const
        {
            Press(TEXT("I"));
            Player.ClickJewelrySlot(Location);
            Press(TEXT("I"));
        }
        void Use(int32 Type) const
        {
            Press(TEXT("I"));
            Player.ClickInventorySlot(Player.GetCarriedItems().Find(Type));
            Player.ClickInventorySlot(INDEX_NONE);
            Press(TEXT("I"));
            Press(TEXT("LeftControl"));
        }
        void CastSustenance() const
        {
            Press(TEXT("NumPadEight"));
            Press(TEXT("NumPadTwo"));
            Press(TEXT("NumPadNine"));
            Press(TEXT("Enter"));
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalConservationItems, "AAOriginal.WAD.Gameplay.ConservationItems",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalConservationItems::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for source conservation equipment"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->SetGameInstance(NewObject<UGameInstance>());
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Player || !Controller) { return false; }
    World->AddController(Controller);
    Controller->Possess(Player);
    const FConservationInputs Keys(*Player);
    Player->CollectItem(Berry);
    Player->Tick(6.0f);
    TestEqual(TEXT("Ordinary source food depletion is one per needs tick"), Player->GetFood(), 1999);
    TestEqual(TEXT("Ordinary source water depletion is one per needs tick"), Player->GetWater(), 1999);
    Keys.Wear(426, Ring1Slot);
    Keys.Wear(427, Ring2Slot);
    TestEqual(TEXT("The actual Bank food ring supplies conservation"), Player->GetStatusPower(FoodConservationStatus), 1);
    TestEqual(TEXT("The actual Bank water ring supplies conservation"), Player->GetStatusPower(WaterConservationStatus), 1);
    Player->ReceiveEncounterHit(500);
    Player->RestoreWadVital(10, -500);
    const int32 Health = Player->GetHealth();
    const int32 Mana = Player->GetMana();
    Player->Tick(6.0f);
    TestEqual(TEXT("Food conservation preserves the existing amount rather than refilling it"), Player->GetFood(), 1999);
    TestEqual(TEXT("Water conservation preserves the existing amount rather than refilling it"), Player->GetWater(), 1999);
    TestEqual(TEXT("Conservation retains the source food-ratio health recovery"), Player->GetHealth(), Health + 17);
    TestEqual(TEXT("Conservation retains the source water-ratio mana recovery"), Player->GetMana(), Mana + 156);
    Keys.Remove(Ring1Slot);
    Player->Tick(6.0f);
    TestEqual(TEXT("Removing only food conservation resumes food depletion"), Player->GetFood(), 1998);
    TestEqual(TEXT("The other ring still conserves water"), Player->GetWater(), 1999);
    Keys.Remove(Ring2Slot);
    Player->Tick(6.0f);
    TestEqual(TEXT("With neither ring food continues to deplete"), Player->GetFood(), 1997);
    TestEqual(TEXT("Removing the water ring resumes water depletion"), Player->GetWater(), 1998);

    Keys.Wear(610, NeckSlot);
    Player->Tick(6.0f);
    TestEqual(TEXT("The original Nourishment amulet conserves food"), Player->GetFood(), 1997);
    TestEqual(TEXT("The original Nourishment amulet conserves water"), Player->GetWater(), 1998);
    Player->CollectItem(807);
    Keys.Use(807);
    TestEqual(TEXT("Nourishment does not cancel a potion's authored dehydration"), Player->GetWater(), 1798);
    TestEqual(TEXT("The cure potion is still consumed"), Player->GetItemCount(807), 0);
    Player->CollectItem(StartingWaterGourd);
    Keys.Use(StartingWaterGourd);
    TestEqual(TEXT("A normal drink still restores water and clamps at the maximum"), Player->GetWater(), MaxFood);
    TestEqual(TEXT("The water gourd is consumed normally"), Player->GetItemCount(StartingWaterGourd), 0);
    Player->RestoreWadVital(7, -500);
    Keys.Use(Berry);
    TestEqual(TEXT("Food still restores its original amount while nourishment is active"), Player->GetFood(), 1697);
    TestEqual(TEXT("The original food consumption remains in force"), Player->GetItemCount(Berry), 0);
    Keys.Wear(427, Ring2Slot);
    Keys.Remove(NeckSlot);
    Player->Tick(6.0f);
    TestEqual(TEXT("Removing Nourishment resumes food depletion"), Player->GetFood(), 1696);
    TestEqual(TEXT("Removing Nourishment does not remove another object's water conservation"), Player->GetWater(), MaxFood);
    Keys.Remove(Ring2Slot);
    Player->Tick(6.0f);
    TestEqual(TEXT("Removing the final conservation owner resumes water depletion"), Player->GetWater(), MaxFood - 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSustenanceSpell, "AAOriginal.WAD.Gameplay.SustenanceSpell",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSustenanceSpell::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for timed source Sustenance"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->SetGameInstance(NewObject<UGameInstance>());
    // Controlled level/Magic make the real rune cast deterministic; this is not campaign advancement.
    for (const TCHAR* Option : {TEXT("WadClass=0"), TEXT("WadCreated=1"), TEXT("WadRuneRules=1"),
        TEXT("WadLevel=100"), TEXT("WadAttributes=25,25,100,25,25,25"), TEXT("WadMaxMana=10000"),
        TEXT("WadMana=10000"), TEXT("WadItems=301,312,313")})
    { World->URL.AddOption(Option); }
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Player || !Controller) { return false; }
    World->AddController(Controller);
    Controller->Possess(Player);
    Player->DispatchBeginPlay();
    const FConservationInputs Keys(*Player);
    if (!TestEqual(TEXT("The source fixture is an Arcane Citizen"), Player->GetCharacterClass(), int32(CitizenClass))) { return false; }
    Keys.CastSustenance();
    TestEqual(TEXT("The source 829 rune spell spends 1000 mana"), Player->GetMana(), 9000);
    TestEqual(TEXT("The actual rune spell applies Eternal Nourishment"), Player->GetStatusPower(EternalNourishmentStatus), 1);
    TestEqual(TEXT("A successful source cast is recorded"), Player->GetSuccessfulCasts(), 1);
    for (int32 Tick = 0; Tick < 66; ++Tick) { Player->Tick(6.0f); }
    TestEqual(TEXT("The source 27999-tick effect remains active after 396 seconds"), Player->GetStatusPower(EternalNourishmentStatus), 1);
    TestEqual(TEXT("Timed nourishment conserves food over repeated needs ticks"), Player->GetFood(), MaxFood);
    TestEqual(TEXT("Timed nourishment conserves water over repeated needs ticks"), Player->GetWater(), MaxFood);
    Player->RestoreWadVital(10, 1000 - Player->GetMana());
    Keys.CastSustenance();
    TestEqual(TEXT("Mana equal to the source spell cost refuses without spending it"), Player->GetMana(), 1000);
    TestEqual(TEXT("A refused recast is not recorded as successful"), Player->GetSuccessfulCasts(), 1);
    Player->Tick(4.1f);
    TestEqual(TEXT("The natural deadline expires and the refused recast did not refresh it"), Player->GetStatusPower(EternalNourishmentStatus), 0);
    Player->Tick(2.0f);
    TestEqual(TEXT("The first needs tick after expiry depletes food again"), Player->GetFood(), MaxFood - 1);
    TestEqual(TEXT("The first needs tick after expiry depletes water again"), Player->GetWater(), MaxFood - 1);
    return true;
}
