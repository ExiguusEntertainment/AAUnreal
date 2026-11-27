#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
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

        bool EnablePlayerTick() const
        {
            APlayerController* Controller = World && Player ? World->SpawnActor<APlayerController>() : nullptr;
            if (!Controller) { return false; }
            World->AddController(Controller);
            Controller->Possess(Player);
            return Controller->IsLocalController();
        }

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalOwnedStatusRuneSpells, "AAOriginal.WAD.Gameplay.OwnedStatusRuneSpells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalOwnedStatusRuneSpells::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const int32 Definitions[] = {16,21,23,24,25,26,27,30,31,32,35,38,40,
        55,56,59,60,61,63,65,66,67,81,83,84,88,89,92,93,94,95,96,97};
    int32 SchoolCounts[3] = {};
    for (const int32 Definition : Definitions)
    {
        const FWadSpell* Spell = Algo::FindByPredicate(SupportedSpells, [&](const auto& Entry)
            { return Entry.SourceDefinition == Definition && Entry.Effect == ESpellEffect::Status; });
        if (!TestNotNull(*FString::Printf(TEXT("Source definition %d has a timed status spell"), Definition), Spell)) { return false; }
        const int32 Class = Spell->School == ESpellSchool::Mage ? MageClass
            : Spell->School == ESpellSchool::Cleric ? PriestClass : CitizenClass;
        ++SchoolCounts[Spell->School == ESpellSchool::Mage ? 0 : Spell->School == ESpellSchool::Cleric ? 1 : 2];
        FUtilitySpellFixture Fixture(Class);
        auto* Player = Fixture.Player;
        if (!TestNotNull(TEXT("Created a source-school character with all nine owned runes"), Player)
            || !TestTrue(TEXT("A local possessed character can advance real status timers"), Fixture.EnablePlayerTick())) { return false; }
        const FString Owner = FWadPlayerEffects::SpellOwner(Definition);
        const EItemStatus Status = EItemStatus(Spell->Status);
        const double Duration = Spell->DurationTicks / double(TicksPerSecond);
        const int32 ManaBefore = Player->GetMana();
        const int32 CastsBefore = Player->GetSuccessfulCasts();
        if (!TestTrue(*FString::Printf(TEXT("Definition %d casts through keypad runes and RightMouse"), Definition), Fixture.Cast(Spell->Runes))) { return false; }
        TestEqual(TEXT("A successful status spell charges its exact source mana cost"), Player->GetMana(), ManaBefore - Spell->Mana);
        TestEqual(TEXT("A successful status spell increments the cast counter"), Player->GetSuccessfulCasts(), CastsBefore + 1);
        TestEqual(TEXT("The actual source spell definition becomes the temporary owner"), Player->GetOwnedEffectRemaining(Owner), Duration);
        TestEqual(TEXT("The live status supplies the exact source power"), Player->GetStatusPower(Status), Spell->Power);
        TestEqual(TEXT("One successful cast creates one owner"), Player->GetOwnedEffectCount(), 1);
        TestTrue(TEXT("The input rune sequence clears after a successful cast"), Player->GetRunes().IsEmpty());

        Player->Tick(1.25f);
        TestEqual(TEXT("The real player tick decreases this source owner's remaining duration"),
            Player->GetOwnedEffectRemaining(Owner), Duration - 1.25);
        const int32 ManaBeforeRefresh = Player->GetMana();
        Fixture.Cast(Spell->Runes);
        TestEqual(TEXT("Recasting refreshes the same owner back to the full source duration"), Player->GetOwnedEffectRemaining(Owner), Duration);
        TestEqual(TEXT("Refresh never duplicates the source definition owner"), Player->GetOwnedEffectCount(), 1);
        TestEqual(TEXT("Refresh never doubles the status power"), Player->GetStatusPower(Status), Spell->Power);
        TestEqual(TEXT("Refresh spends one further full source cost"), Player->GetMana(), ManaBeforeRefresh - Spell->Mana);
        TestEqual(TEXT("Refresh counts as another successful cast"), Player->GetSuccessfulCasts(), CastsBefore + 2);

        Player->RestoreWadVital(10, Spell->Mana - Player->GetMana());
        Fixture.Cast(Spell->Runes);
        TestEqual(TEXT("Exact-cost mana is insufficient under the source strict comparison"), Player->GetMana(), Spell->Mana);
        TestEqual(TEXT("A refused cast does not increment the successful count"), Player->GetSuccessfulCasts(), CastsBefore + 2);
        TestEqual(TEXT("A refused cast leaves the previous owner and duration intact"), Player->GetOwnedEffectRemaining(Owner), Duration);
        TestEqual(TEXT("A refused cast creates no additional owner"), Player->GetOwnedEffectCount(), 1);
        TestEqual(TEXT("Casting preserves the nine physical rune objects"), Player->GetCarriedItems().Num(), 9);
    }
    TestEqual(TEXT("All thirteen newly supported Mage status definitions were exercised"), SchoolCounts[0], 13);
    TestEqual(TEXT("All eleven newly supported Cleric status definitions were exercised"), SchoolCounts[1], 11);
    TestEqual(TEXT("All nine newly supported Arcane status definitions were exercised"), SchoolCounts[2], 9);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalIdentificationRuneScopes, "AAOriginal.WAD.Gameplay.IdentificationRuneScopes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalIdentificationRuneScopes::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (const int32 Definition : {105, 106, 107})
    {
        const FWadSpell* Spell = Algo::FindByPredicate(SupportedSpells,
            [&](const auto& Entry) { return Entry.SourceDefinition == Definition; });
        if (!TestNotNull(TEXT("Each original Identify spell is registered"), Spell)) { return false; }
        FUtilitySpellFixture Fixture(Spell->School == ESpellSchool::Mage ? MageClass : PriestClass);
        auto* Player = Fixture.Player;
        if (!TestNotNull(TEXT("Created character for the original Identify rune path"), Player)) { return false; }
        Player->CollectItem(417);
        Player->CollectItem(418);
        Fixture.Press(EKeys::I);
        Player->ClickInventorySlot(Player->GetCarriedItems().Find(417));
        Player->ClickJewelrySlot(Ring1Slot);
        Fixture.Press(EKeys::I);
        TestTrue(TEXT("The equipped fixture object actually occupies a worn ring slot"),
            Player->GetCarriedItems().IsValidIndex(Player->GetJewelryInventorySlot(Ring1Slot))
            && Player->GetCarriedItems()[Player->GetJewelryInventorySlot(Ring1Slot)] == 417);
        Player->InitializeItemIdentification(false);
        TestFalse(TEXT("The worn ring starts unidentified"), Player->HasIdentifiedItemType(417));
        TestFalse(TEXT("The carried ring starts unidentified"), Player->HasIdentifiedItemType(418));
        const int32 Casts = Player->GetSuccessfulCasts();

        Player->RestoreWadVital(10, Spell->Mana - Player->GetMana());
        if (!TestTrue(TEXT("Identify exact-cost refusal uses ordinary rune keys and RightMouse"), Fixture.Cast(Spell->Runes))) { return false; }
        TestEqual(TEXT("A refused Identify spell spends no mana"), Player->GetMana(), Spell->Mana);
        TestEqual(TEXT("A refused Identify spell does not count as successful"), Player->GetSuccessfulCasts(), Casts);
        TestFalse(TEXT("A refused Identify spell does not reveal equipped objects"), Player->HasIdentifiedItemType(417));
        TestFalse(TEXT("A refused Identify spell does not reveal bag objects"), Player->HasIdentifiedItemType(418));

        Player->RestoreWadVital(10, Player->GetMaxMana() - Player->GetMana());
        const int32 Mana = Player->GetMana();
        while (!Player->GetRunes().IsEmpty()) { Fixture.Press(EKeys::BackSpace); }
        if (!TestTrue(TEXT("Identify casts through its actual source-school rune inputs"), Fixture.Cast(Spell->Runes))) { return false; }
        TestEqual(TEXT("Identify spends its full source cost"), Player->GetMana(), Mana - Spell->Mana);
        TestEqual(TEXT("Identify increments the successful cast count"), Player->GetSuccessfulCasts(), Casts + 1);
        TestTrue(TEXT("Every Identify scope reveals the worn ring"), Player->HasIdentifiedItemType(417));
        const bool bAll = Spell->Effect == ESpellEffect::IdentifyAll;
        TestEqual(TEXT("Only Identify All reveals the unready bag object"), Player->HasIdentifiedItemType(418), bAll);
        TestEqual(TEXT("Identification displays the original scope-specific message"), Player->GetInteractionMessage(),
            FString(bAll ? TEXT("You understand your inventory better!") : TEXT("You understand your equipment better!")));
        TestEqual(TEXT("Instant identification creates no timed owner"), Player->GetOwnedEffectCount(), 0);
        TestEqual(TEXT("Identification preserves all carried items"), Player->GetCarriedItems().Num(), 11);
    }
    return true;
}
