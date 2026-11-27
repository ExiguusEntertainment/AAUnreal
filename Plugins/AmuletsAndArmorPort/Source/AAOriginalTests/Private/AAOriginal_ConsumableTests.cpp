#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "NativeWadPlayer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPoisonCures, "AAOriginal.WAD.Gameplay.PoisonCures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalPoisonCures::RunTest(const FString& Parameters)
{
    // The saved item catalog must have been refreshed from the original INVDESC files.
    // Exercise actual inventory selection and the bound ready-hand input, rather than
    // supplying a hand-written substitute for the imported potion rules.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for original poison cures"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    if (!TestNotNull(TEXT("Player for original poison cures"), Player)) { return false; }
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
    const auto Ready = [&](int32 Type)
    {
        if (!Press(TEXT("I"))) { return false; }
        Player->ClickInventorySlot(Player->GetCarriedItems().Find(Type));
        Player->ClickInventorySlot(INDEX_NONE);
        return Press(TEXT("I")) && Player->GetReadyItem() == Type;
    };
    Player->ReceiveTrapPoison(50);
    if (!TestEqual(TEXT("A source poison hit gives the fixture five poison"), Player->GetPoisonLevel(), 5)) { return false; }
    const int32 Health = Player->GetHealth();
    const int32 Mana = Player->GetMana();
    const int32 Water = Player->GetWater();
    Player->CollectItem(NativeWadRules::Berry);
    for (const int32 Expected : {3, 2, 1, 1})
    {
        Player->CollectItem(806);
        if (!TestTrue(TEXT("Ready the original weaker cure from inventory"), Ready(806))) { return false; }
        Press(TEXT("LeftControl"));
        TestEqual(TEXT("The source weaker cure retains the upper half of odd poison"), Player->GetPoisonLevel(), Expected);
        TestEqual(TEXT("The weaker cure is consumed once"), Player->GetItemCount(806), 0);
        TestEqual(TEXT("The source partial-cure message is shown"), Player->GetInteractionMessage(), FString(TEXT("The poison in your body is less.")));
    }
    TestEqual(TEXT("Four weaker cures each cost 200 water"), Player->GetWater(), FMath::Max(0, Water - 800));
    Player->CollectItem(807);
    if (!TestTrue(TEXT("Ready the original full cure from inventory"), Ready(807))) { return false; }
    Press(TEXT("LeftControl"));
    TestEqual(TEXT("The original full cure clears the final point"), Player->GetPoisonLevel(), 0);
    TestEqual(TEXT("The full cure is consumed once"), Player->GetItemCount(807), 0);
    TestEqual(TEXT("The source full-cure message is shown"), Player->GetInteractionMessage(), FString(TEXT("Your body is cleansed of all poison.")));
    TestEqual(TEXT("Curing does not restore health"), Player->GetHealth(), Health);
    TestEqual(TEXT("Curing does not spend or restore mana"), Player->GetMana(), Mana);
    TestEqual(TEXT("Curing leaves other inventory alone"), Player->GetItemCount(NativeWadRules::Berry), 1);

    // Healthy uses still consume the item and its water cost, including at zero water.
    for (int32 Index = 0; Index <= Water / 200; ++Index)
    {
        Player->CollectItem(807);
        if (!TestTrue(TEXT("Ready a full cure while already healthy"), Ready(807))) { return false; }
        Press(TEXT("LeftControl"));
        TestEqual(TEXT("A healthy character can still consume the source potion"), Player->GetItemCount(807), 0);
    }
    TestEqual(TEXT("Potion dehydration stops at zero"), Player->GetWater(), 0);
    TestEqual(TEXT("The healthy use keeps poison at zero"), Player->GetPoisonLevel(), 0);
    return true;
}
