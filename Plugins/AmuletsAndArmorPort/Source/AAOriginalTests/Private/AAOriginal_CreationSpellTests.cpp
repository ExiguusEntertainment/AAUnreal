#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"

namespace
{
    struct FCreationSpellFixture
    {
        UGameInstance* Instance = nullptr;
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;

        explicit FCreationSpellFixture(const TCHAR* SavedOptions = nullptr)
        {
            Instance = NewObject<UGameInstance>(GEngine);
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            World->URL = FURL(nullptr, SavedOptions ? SavedOptions : TEXT("/Game/Maps/L30?WadClass=4?WadCreated=1?WadRuneRules=1")
                TEXT("?WadLevel=100?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=10000")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadReady=-1?WadItems=309,310,311,312,313,314,315,316,317"), TRAVEL_Absolute);
            Player = World->SpawnActor<ANativeWadPlayer>();
            if (!Player) { return; }
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }

        ~FCreationSpellFixture()
        {
            if (Instance) { Instance->Shutdown(); }
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Valid() const { return World && Player && Input; }
        bool Press(FKey Key) const
        {
            if (!Input) { return false; }
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
    };

    const NativeWadRules::FWadSpell* CreationSpell(int32 Definition)
    {
        return Algo::FindByPredicate(NativeWadRules::SupportedSpells,
            [&](const auto& Spell) { return Spell.SourceDefinition == Definition && Spell.Effect == NativeWadRules::ESpellEffect::CreateObject; });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCreationRuneSpells, "AAOriginal.WAD.Gameplay.CreationRuneSpells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalCreationRuneSpells::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (const int32 Definition : {86, 87, 90})
    {
        const FWadSpell* Spell = CreationSpell(Definition);
        if (!TestNotNull(TEXT("The source creation definition is registered"), Spell)) { return false; }
        TArray<FString> RawLines, Fields;
        const FString Source = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC")
            / FString::Printf(TEXT("SPL%05d.TXT"), Definition);
        if (!TestTrue(TEXT("Read the actual source creation spell resource"), FFileHelper::LoadFileToStringArray(RawLines, *Source))) { return false; }
        for (const FString& Line : RawLines)
        {
            const FString Value = Line.TrimStartAndEnd();
            if (!Value.IsEmpty() && !Value.StartsWith(TEXT("#"))) { Fields.Add(Value); }
        }
        const auto Field = [&](const TCHAR* Name)
        {
            const int32 Index = Fields.Find(Name);
            return Index != INDEX_NONE && Fields.IsValidIndex(Index + 1) ? Fields[Index + 1] : FString();
        };
        FString Runes = Field(TEXT("ACCESS CODE"));
        Runes.ReplaceInline(TEXT(","), TEXT("")); Runes.ReplaceInline(TEXT("0"), TEXT("")); Runes.ReplaceInline(TEXT(" "), TEXT(""));
        TestEqual(TEXT("Creation runes match the original definition"), FString(Spell->Runes), Runes);
        TestEqual(TEXT("Creation uses the original Cleric school"), Field(TEXT("SPELL SYSTEM")), FString(TEXT("CLERIC")));
        TestTrue(TEXT("Native school agrees with the original"), Spell->School == ESpellSchool::Cleric);
        TestEqual(TEXT("Creation uses the original object effect"), Field(TEXT("EFFECT TYPE")), FString(TEXT("CREATE OBJECT")));
        TestEqual(TEXT("The created type is the original source power"), Spell->Power, FCString::Atoi(*Field(TEXT("SPELL POWER"))));
        TestEqual(TEXT("The mana cost matches the original"), Spell->Mana, FCString::Atoi(*Field(TEXT("SPELL COST"))));
        TestEqual(TEXT("The casting difficulty matches the original"), Spell->Hardness, FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
        TestEqual(TEXT("Instant source creation has no duration"), Spell->DurationTicks, 0);

        FCreationSpellFixture Fixture;
        if (!TestTrue(TEXT("Real source-school caster is ready"), Fixture.Valid())) { return false; }
        auto* Player = Fixture.Player;
        const UNativeWadHudData* Catalog = static_cast<UNativeWadHudData*>(LoadObject<UObject>(nullptr, UNativeWadHudData::ObjectPath));
        if (!TestNotNull(TEXT("Imported ordinary item art/rules are available"), Catalog)) { return false; }
        const FWadInventoryDetails* Rules = Catalog->ItemDetails.Find(Spell->Power);
        if (!TestNotNull(TEXT("The created type has its original use rules"), Rules)) { return false; }
        TestNotNull(TEXT("The created object has original cursor/inventory art"), Catalog->Items.FindRef(Spell->Power).Get());
        TestFalse(TEXT("The created object has saved world art for normal dropping"), Rules->WorldMaterial.IsNull());
        Player->InitializeItemIdentification(false);
        TestEqual(TEXT("The spell grants no items before the ordinary cast input"), Player->GetCarriedItems().Num(), 9);
        TestEqual(TEXT("The cursor starts empty"), Player->GetMouseHandItem(), 0);
        TestEqual(TEXT("The created type starts unowned"), Player->GetItemCount(Spell->Power), 0);
        const int32 Health = Player->GetHealth(), Food = Player->GetFood(), Water = Player->GetWater();
        const int32 Mana = Player->GetMana(), Casts = Player->GetSuccessfulCasts();
        if (!TestTrue(TEXT("Create uses physical keypad rune keys and RightMouse"), Fixture.Cast(Spell->Runes))) { return false; }
        TestEqual(TEXT("The source type appears directly in the persistent mouse hand"), Player->GetMouseHandItem(), Spell->Power);
        TestEqual(TEXT("Exactly one ordinary object is owned"), Player->GetItemCount(Spell->Power), 1);
        TestEqual(TEXT("Creation adds one object without replacing a rune"), Player->GetCarriedItems().Num(), 10);
        TestEqual(TEXT("Creation does not auto-ready the new object"), Player->GetReadyItem(), 0);
        TestEqual(TEXT("Creation charges the full original mana cost"), Player->GetMana(), Mana - Spell->Mana);
        TestEqual(TEXT("Creation counts as one successful spell"), Player->GetSuccessfulCasts(), Casts + 1);
        TestEqual(TEXT("Creation does not consume the result as a healing spell"), Player->GetHealth(), Health);
        TestEqual(TEXT("Creation does not feed the character before using the result"), Player->GetFood(), Food);
        TestEqual(TEXT("Creation does not drink the result"), Player->GetWater(), Water);
        TestFalse(TEXT("Creation alone does not identify its source type"), Player->HasIdentifiedItemType(Spell->Power));
        TestEqual(TEXT("Instant creation has no timed effect owner"), Player->GetOwnedEffectCount(), 0);

        const TArray<int32> Items = Player->GetCarriedItems();
        const int32 Held = Player->GetMouseHandSlot(), ManaBeforeBlocked = Player->GetMana();
        Fixture.Cast(Spell->Runes);
        TestTrue(TEXT("An occupied hand is never overwritten or silently stashed"), Items == Player->GetCarriedItems() && Player->GetMouseHandSlot() == Held);
        TestEqual(TEXT("Source occupied-hand refusal happens after mana is paid"), Player->GetMana(), ManaBeforeBlocked - Spell->Mana);
        TestEqual(TEXT("A successfully cast but blocked create still counts as a spell"), Player->GetSuccessfulCasts(), Casts + 2);
        TestEqual(TEXT("The original occupied-hand message survives generic spell feedback"), Player->GetInteractionMessage(), FString(TEXT("Can't create while item is in hand.")));
        TestFalse(TEXT("Blocked creation does not identify the retained object"), Player->HasIdentifiedItemType(Spell->Power));

        Player->StoreMouseHandInInventory();
        Player->RestoreWadVital(10, Spell->Mana - Player->GetMana());
        Fixture.Cast(Spell->Runes);
        TestEqual(TEXT("Exact-cost refusal spends no mana"), Player->GetMana(), Spell->Mana);
        TestEqual(TEXT("Exact-cost refusal creates no new object in an empty hand"), Player->GetMouseHandItem(), 0);
        TestTrue(TEXT("Exact-cost refusal preserves the existing inventory"), Items == Player->GetCarriedItems());
        TestEqual(TEXT("Exact-cost refusal does not increment successful casts"), Player->GetSuccessfulCasts(), Casts + 2);

        // A normal bag take, ready swap and use consume the ordinary created item.
        if (!TestTrue(TEXT("The created object can be taken from the bag"), Player->TakeInventoryItem(Held))
            || !TestTrue(TEXT("The created object can be readied normally"), Player->SwapMouseAndReady())
            || !TestTrue(TEXT("Use goes through the actual LeftControl binding"), Fixture.Press(EKeys::LeftControl))) { return false; }
        TestEqual(TEXT("Using the created object consumes exactly one ordinary item"), Player->GetItemCount(Spell->Power), 0);
        TestEqual(TEXT("Using it restores the original nine-rune ownership count"), Player->GetCarriedItems().Num(), 9);
        TestEqual(TEXT("The created item's normal health effect is retained"), Player->GetHealth(), FMath::Min(Player->GetMaxHealth(), Health + Rules->Health));
        TestEqual(TEXT("The created item's normal food effect is retained"), Player->GetFood(), FMath::Clamp(Food + Rules->Food, 0, MaxFood));
        TestEqual(TEXT("The created item's normal water effect is retained"), Player->GetWater(), FMath::Clamp(Water + Rules->Water, 0, MaxFood));
        TestTrue(TEXT("Normal original use, rather than creation, teaches this type"), Player->HasIdentifiedItemType(Spell->Power));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCreationHandPersistence, "AAOriginal.WAD.Gameplay.CreationHandPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalCreationHandPersistence::RunTest(const FString& Parameters)
{
    for (const int32 Definition : {86, 87, 90})
    {
        const auto* Spell = CreationSpell(Definition);
        FCreationSpellFixture Fixture;
        if (!TestNotNull(TEXT("Source creation spell for saved hand"), Spell) || !TestTrue(TEXT("Standalone save fixture is ready"), Fixture.Valid())) { return false; }
        auto* Player = Fixture.Player;
        Player->InitializeItemIdentification(false);
        if (!TestTrue(TEXT("The saved object originates in an actual source rune cast"), Fixture.Cast(Spell->Runes))) { return false; }
        // Town permits saves without requiring a physical map floor. Switch game
        // mode only after binding/casting through the normal in-level inputs.
        const FURL TownURL(nullptr, TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"), TRAVEL_Absolute);
        if (!TestTrue(TEXT("Use the real Town save eligibility rule"), Fixture.World->SetGameMode(TownURL))) { return false; }
        const FString Slot = TEXT("NativeCreationHandTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot, 0); };
        if (!TestTrue(TEXT("The actual checkpoint writer saves a held created object"), Player->SaveWadCheckpoint(Slot))) { return false; }
        USaveGame* RawSave = UGameplayStatics::LoadGameFromSlot(Slot, 0);
        if (!TestNotNull(TEXT("The actual checkpoint can be read back from disk"), RawSave)) { return false; }
        // This private reflected runtime class is not exported from AAOriginal;
        // avoid a cross-DLL StaticClass reference in the separate test module.
        const auto* Saved = static_cast<UNativeWadCheckpoint*>(RawSave);
        const FString SavedOptions = Saved->LevelPackage + TEXT("?") + Saved->PlayerOptions;
        const FURL URL(nullptr, *SavedOptions, TRAVEL_Absolute);
        TestEqual(TEXT("Save options retain the held inventory index"), FCString::Atoi(URL.GetOption(TEXT("WadMouseHand="), TEXT("-1"))), Player->GetMouseHandSlot());
        TestFalse(TEXT("Saving an unidentified created object does not reveal it"), Saved->IdentifiedItemTypes.Contains(Spell->Power));
        const TSet<int32> SavedKnowledge = Saved->IdentifiedItemTypes;
        const bool bSavedKnowledgeRecorded = Saved->bItemIdentificationRecorded;
        FCreationSpellFixture Restored(*SavedOptions);
        if (!TestTrue(TEXT("A new player restores the actual saved travel options"), Restored.Valid())) { return false; }
        Restored.Player->RestoreItemIdentification(SavedKnowledge, bSavedKnowledgeRecorded);
        TestEqual(TEXT("The saved exact type returns to the mouse hand"), Restored.Player->GetMouseHandItem(), Spell->Power);
        TestEqual(TEXT("The held index survives a new player instance"), Restored.Player->GetMouseHandSlot(), Player->GetMouseHandSlot());
        TestTrue(TEXT("The save/reload did not copy or lose ordinary inventory objects"), Restored.Player->GetCarriedItems() == Player->GetCarriedItems());
        TestFalse(TEXT("Reload preserves the created object's unidentified status"), Restored.Player->HasIdentifiedItemType(Spell->Power));
        TestEqual(TEXT("Reload retains the mana paid for creation"), Restored.Player->GetMana(), Player->GetMana());
        TestEqual(TEXT("Reload retains the successful cast count"), Restored.Player->GetSuccessfulCasts(), Player->GetSuccessfulCasts());
    }
    return true;
}
