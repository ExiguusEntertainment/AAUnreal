#include "Misc/AutomationTest.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"

namespace
{
    struct FUtilitySpellFixture
    {
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;

        explicit FUtilitySpellFixture(int32 Class)
        {
            World = UWorld::CreateWorld(EWorldType::Game, false);
            if (!World) { return; }
            World->SetGameInstance(NewObject<UGameInstance>());
            World->URL.AddOption(*FString::Printf(TEXT("WadClass=%d"), Class));
            World->URL.AddOption(TEXT("WadCreated=1"));
            World->URL.AddOption(TEXT("WadRuneRules=1"));
            World->URL.AddOption(TEXT("WadLevel=100"));
            World->URL.AddOption(TEXT("WadAttributes=100,100,100,100,100,100"));
            World->URL.AddOption(TEXT("WadMaxHealth=30000"));
            World->URL.AddOption(TEXT("WadHealth=10000"));
            World->URL.AddOption(TEXT("WadMaxMana=30000"));
            World->URL.AddOption(TEXT("WadMana=30000"));
            FString Items;
            for (int32 Rune = 1; Rune <= 9; ++Rune)
            {
                if (!Items.IsEmpty()) { Items += TEXT(","); }
                Items += FString::FromInt(NativeWadRules::RuneItemForClass(Class, Rune));
            }
            World->URL.AddOption(*(TEXT("WadItems=") + Items));
            Player = World->SpawnActor<ANativeWadPlayer>();
            if (!Player) { return; }
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }

        ~FUtilitySpellFixture() { if (World) { World->DestroyWorld(false); } }

        bool Press(const FKey& Key) const
        {
            if (!Input) { return false; }
            for (const FInputKeyBinding& Binding : Input->KeyBindings)
            {
                if (Binding.KeyEvent == IE_Pressed && Binding.Chord == FInputChord(Key))
                { Binding.KeyDelegate.Execute(Key); return true; }
            }
            return false;
        }

        bool Cast(const TCHAR* Runes) const
        {
            const FKey Keys[] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree,
                EKeys::NumPadFour, EKeys::NumPadFive, EKeys::NumPadSix,
                EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
            for (const TCHAR* Rune = Runes; *Rune; ++Rune)
            {
                if (*Rune < '1' || *Rune > '9' || !Press(Keys[*Rune - '1'])) { return false; }
            }
            return Press(EKeys::RightMouseButton);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalUtilityRuneSpells, "AAOriginal.WAD.Gameplay.UtilityRuneSpells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalUtilityRuneSpells::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    // The source's rand()%100 comparison is inclusive. Clamping a negative threshold
    // retains a one-percent success chance instead of making a difficult spell impossible.
    TestEqual(TEXT("Negative source casting difficulty clamps to zero"), SpellCastingThreshold(10, -40, 1), 0);
    TestTrue(TEXT("Roll zero still succeeds at the clamped difficulty"), SpellRollSucceeds(10, -40, 1, 0));
    TestFalse(TEXT("Roll one fizzles at the clamped difficulty"), SpellRollSucceeds(10, -40, 1, 1));
    TestTrue(TEXT("The exact positive threshold succeeds"), SpellRollSucceeds(35, 50, 1, 87));
    TestFalse(TEXT("The first roll above that threshold fizzles"), SpellRollSucceeds(35, 50, 1, 88));
    TestTrue(TEXT("A threshold above 99 permits every source d100 roll"), SpellRollSucceeds(100, -40, 100, 99));

    {
        FUtilitySpellFixture Fixture(PriestClass);
        ANativeWadPlayer* Player = Fixture.Player;
        if (!TestNotNull(TEXT("Created Priest for actual utility spell inputs"), Player)) { return false; }
        Player->ReceiveTrapPoison(50);
        if (!TestEqual(TEXT("Poison fixture starts with five source poison"), Player->GetPoisonLevel(), 5)) { return false; }
        const int32 HealthBefore = Player->GetHealth();
        const int32 WaterBefore = Player->GetWater();
        int32 ManaBefore = Player->GetMana();
        for (int32 Expected : {3, 2, 1, 1})
        {
            if (!TestTrue(TEXT("Purify Blood casts through owned keypad runes and right mouse"), Fixture.Cast(TEXT("653")))) { return false; }
            TestEqual(TEXT("Purify Blood retains the source upper half, including the final poison point"), Player->GetPoisonLevel(), Expected);
            TestEqual(TEXT("Purify Blood spends 750 mana per successful cast"), Player->GetMana(), ManaBefore - 750);
            TestEqual(TEXT("Rune curing does not include a potion's water penalty"), Player->GetWater(), WaterBefore);
            TestEqual(TEXT("Rune curing does not heal"), Player->GetHealth(), HealthBefore);
            TestEqual(TEXT("Purify Blood presents the source partial-cure message"), Player->GetInteractionMessage(), FString(TEXT("The poison in your body is less.")));
            TestTrue(TEXT("Successful casts clear the entered rune sequence"), Player->GetRunes().IsEmpty());
            ManaBefore = Player->GetMana();
        }
        Fixture.Cast(TEXT("415"));
        TestEqual(TEXT("Heal adds its source 5000 health"), Player->GetHealth(), HealthBefore + 5000);
        TestEqual(TEXT("Heal spends its source 8000 mana"), Player->GetMana(), ManaBefore - 8000);
        TestEqual(TEXT("Heal does not remove poison"), Player->GetPoisonLevel(), 1);
        Player->RestoreWadVital(9, Player->GetMaxHealth() - Player->GetHealth() - 100);
        Fixture.Cast(TEXT("415"));
        TestEqual(TEXT("Heal saturates at the character's maximum health"), Player->GetHealth(), Player->GetMaxHealth());
        Player->RestoreWadVital(10, 8000 - Player->GetMana());
        const int32 Casts = Player->GetSuccessfulCasts();
        Fixture.Cast(TEXT("415"));
        TestEqual(TEXT("Mana equal to Heal's cost refuses without spending"), Player->GetMana(), 8000);
        TestEqual(TEXT("An exhausted cast does not count as successful"), Player->GetSuccessfulCasts(), Casts);
        Fixture.Cast(TEXT("154"));
        TestEqual(TEXT("Mage Shield is unavailable to a Cleric despite owned numeric runes"), Player->GetStatusPower(SpellShieldStatus), 0);
        TestEqual(TEXT("A different school's code does not spend mana"), Player->GetMana(), 8000);
        TestEqual(TEXT("Spell use retains all nine owned runes"), Player->GetCarriedItems().Num(), 9);
    }
    {
        FUtilitySpellFixture Fixture(MageClass);
        ANativeWadPlayer* Player = Fixture.Player;
        if (!TestNotNull(TEXT("Created Mage for Shield and Fly"), Player)) { return false; }
        Fixture.Cast(TEXT("154"));
        TestEqual(TEXT("Mage Shield supplies source forty-percent protection"), Player->GetStatusPower(SpellShieldStatus), 40);
        TestEqual(TEXT("Mage Shield costs 2000 mana"), Player->GetMana(), 28000);
        Fixture.Cast(TEXT("167"));
        TestTrue(TEXT("Mage Fly activates the existing flight state"), Player->CanFly());
        TestEqual(TEXT("Mage Fly costs 7000 mana"), Player->GetMana(), 21000);
        Player->RestoreWadVital(10, 7000 - Player->GetMana());
        const int32 Casts = Player->GetSuccessfulCasts();
        Fixture.Cast(TEXT("167"));
        TestEqual(TEXT("Mana equal to Fly's cost refuses without spending"), Player->GetMana(), 7000);
        TestEqual(TEXT("A refused Fly cast does not count as successful"), Player->GetSuccessfulCasts(), Casts);
    }
    {
        FUtilitySpellFixture Fixture(CitizenClass);
        ANativeWadPlayer* Player = Fixture.Player;
        if (!TestNotNull(TEXT("Created Arcane-school character for Feather Fall"), Player)) { return false; }
        Fixture.Cast(TEXT("87"));
        TestTrue(TEXT("Arcane Feather Fall activates the existing falling protection"), Player->HasFeatherfall());
        TestEqual(TEXT("Arcane Feather Fall costs 250 mana"), Player->GetMana(), 29750);
        Player->RestoreWadVital(10, 250 - Player->GetMana());
        const int32 Casts = Player->GetSuccessfulCasts();
        Fixture.Cast(TEXT("87"));
        TestEqual(TEXT("Mana equal to Feather Fall's cost refuses without spending"), Player->GetMana(), 250);
        TestEqual(TEXT("A refused Feather Fall does not count as successful"), Player->GetSuccessfulCasts(), Casts);
    }
    return true;
}
