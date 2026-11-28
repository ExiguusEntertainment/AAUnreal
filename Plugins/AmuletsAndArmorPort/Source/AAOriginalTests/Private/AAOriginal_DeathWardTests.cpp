#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadRules.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    struct FDeathWardFixture
    {
        UGameInstance* Instance = nullptr;
        UWorld* World = nullptr;
        ANativeWadPlayer* Player = nullptr;
        UInputComponent* Input = nullptr;
        explicit FDeathWardFixture(const TCHAR* SavedOptions = nullptr, const TCHAR* Extra = TEXT(""), bool bLevel = false)
        {
            Instance = NewObject<UGameInstance>(GEngine);
            Instance->InitializeStandalone();
            World = Instance->GetWorld();
            if (!World) { return; }
            const FString Options = SavedOptions ? FString(SavedOptions) : FString(TEXT("/Game/Maps/L30?WadClass=2?WadCreated=1?WadRuneRules=1")
                TEXT("?WadLevel=100?WadAttributes=100,100,100,100,100,100?WadMaxHealth=30000?WadHealth=100")
                TEXT("?WadMaxMana=30000?WadMana=30000?WadFood=0?WadWater=0?WadReady=-1?WadItems=300,301,302,303,304,305,306,307,308"));
            World->URL = FURL(nullptr, *(Options + Extra), TRAVEL_Absolute);
            if (bLevel && !World->SpawnActor<ANativeWadLevel>()) { return; }
            Player = World->SpawnActor<ANativeWadPlayer>(FVector(100, 0, 32), FRotator::ZeroRotator);
            APlayerController* Controller = World->SpawnActor<APlayerController>();
            if (!Player || !Controller) { return; }
            World->AddController(Controller);
            Controller->Possess(Player);
            Player->DispatchBeginPlay();
            Input = NewObject<UInputComponent>(Player);
            Player->SetupPlayerInputComponent(Input);
        }
        ~FDeathWardFixture()
        {
            if (Instance) { Instance->Shutdown(); }
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Valid() const { return Player && Input; }
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
        bool Cast(const TCHAR* Runes = TEXT("924")) const
        {
            const FKey Keys[] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
                EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
            const int32 Previous = Player->GetRunes().Len();
            for (int32 Index = 0; Index < Previous; ++Index) { if (!Press(EKeys::BackSpace)) { return false; } }
            for (const TCHAR* Rune = Runes; *Rune; ++Rune)
            { if (*Rune < TEXT('1') || *Rune > TEXT('9') || !Press(Keys[*Rune - TEXT('1')])) { return false; } }
            return Press(EKeys::RightMouseButton);
        }
        bool WearShieldRing() const
        {
            if (!Player->CollectItem(415) || !Player->TakeInventoryItem(Player->GetCarriedItems().Num() - 1)) { return false; }
            Player->ClickJewelrySlot(NativeWadRules::Ring1Slot);
            return Player->GetStatusPower(NativeWadRules::SpellShieldStatus) == 5;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalDeathWardDamage, "AAOriginal.WAD.Gameplay.DeathWardDamage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalDeathWardDamage::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const auto* Spell = Algo::FindByPredicate(SupportedSpells, [](const auto& Entry) { return Entry.SourceDefinition == 12; });
    if (!TestNotNull(TEXT("Original spell12 is supported"), Spell)) { return false; }
    TArray<FString> Raw, Fields;
    if (!TestTrue(TEXT("Read the exact original Death Ward definition"), FFileHelper::LoadFileToStringArray(Raw,
        *(FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC/SPL00012.TXT"))))) { return false; }
    for (const FString& Line : Raw)
    {
        const FString Value = Line.TrimStartAndEnd();
        if (!Value.IsEmpty() && !Value.StartsWith(TEXT("#"))) { Fields.Add(Value); }
    }
    const auto Field = [&](const TCHAR* Name)
    {
        const int32 Index = Fields.Find(Name);
        return Index != INDEX_NONE && Fields.IsValidIndex(Index + 1) ? Fields[Index + 1] : FString();
    };
    TestEqual(TEXT("Source Death Ward uses Mage runes924"), Field(TEXT("ACCESS CODE")), FString(TEXT("9,2,4,0")));
    TestTrue(TEXT("Native effect and school match"), Spell->School == ESpellSchool::Mage
        && Spell->Effect == ESpellEffect::Status && Spell->Status == DeathWardStatus);
    TestEqual(TEXT("Mana matches the original source"), Spell->Mana, FCString::Atoi(*Field(TEXT("SPELL COST"))));
    TestEqual(TEXT("Hardness matches the original source"), Spell->Hardness, FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
    TestEqual(TEXT("Duration matches the original source"), Spell->DurationTicks, FCString::Atoi(*Field(TEXT("SPELL DURATION"))));

    FDeathWardFixture Exact;
    if (!TestTrue(TEXT("Exact-lethal fixture is ready"), Exact.Valid()) || !Exact.Cast()) { return false; }
    TestEqual(TEXT("Ordinary rune input pays the original1000 mana"), Exact.Player->GetMana(), 29000);
    TestEqual(TEXT("The original definition owns the30-second ward"), Exact.Player->GetOwnedEffectRemaining(TEXT("spell:12")), 30.0);
    TestEqual(TEXT("A successful ward cast is counted"), Exact.Player->GetSuccessfulCasts(), 1);
    Exact.Player->ReceiveEncounterHit(100);
    TestEqual(TEXT("Exact-lethal damage reaches real zero health"), Exact.Player->GetHealth(), 0);
    TestFalse(TEXT("An active ward leaves zero-health player alive"), Exact.Player->IsDefeated());
    TestEqual(TEXT("Exact-lethal damage is not strict overkill and charges no ward mana"), Exact.Player->GetMana(), 29000);
    Exact.Player->ReceiveEncounterHit(50);
    TestEqual(TEXT("A further hit at zero charges its full health amount"), Exact.Player->GetMana(), 28950);
    TestFalse(TEXT("A second hit at zero does not consume or defeat the ward"), Exact.Player->IsDefeated());
    Exact.Player->Tick(1.25f);
    TestEqual(TEXT("Timers still advance at zero health"), Exact.Player->GetOwnedEffectRemaining(TEXT("spell:12")), 28.75);
    Exact.Cast();
    TestEqual(TEXT("Recasting while alive at zero refreshes the same owner"), Exact.Player->GetOwnedEffectRemaining(TEXT("spell:12")), 30.0);
    TestEqual(TEXT("Recasting leaves one owner"), Exact.Player->GetOwnedEffectCount(), 1);
    TestEqual(TEXT("Recasting at zero spends its normal cost"), Exact.Player->GetMana(), 27950);
    Exact.Player->RestoreWadVital(10, 1000 - Exact.Player->GetMana());
    Exact.Player->Tick(0.5f);
    Exact.Cast();
    TestEqual(TEXT("Exact mana cost refuses without spending at zero health"), Exact.Player->GetMana(), 1000);
    TestEqual(TEXT("An exhausted cast does not refresh the ward"), Exact.Player->GetOwnedEffectRemaining(TEXT("spell:12")), 29.5);

    FDeathWardFixture Overkill;
    if (!Overkill.Valid() || !Overkill.Cast()) { return false; }
    Overkill.Player->ReceiveEncounterHit(101);
    TestEqual(TEXT("Overkill charges the full101, not only excess1"), Overkill.Player->GetMana(), 28899);
    Overkill.Player->RestoreWadVital(10, -Overkill.Player->GetMana());
    Overkill.Player->ReceiveEncounterHit(500);
    TestEqual(TEXT("Ward mana loss saturates at zero"), Overkill.Player->GetMana(), 0);
    TestFalse(TEXT("An active source ward survives even with no mana"), Overkill.Player->IsDefeated());
    TestEqual(TEXT("Ward protection never substitutes one health point"), Overkill.Player->GetHealth(), 0);

    FDeathWardFixture Typed;
    if (!Typed.Valid() || !Typed.WearShieldRing() || !Typed.Cast()) { return false; }
    const int32 BeforeMana = Typed.Player->GetMana();
    const uint8 Mask = NormalDamage | DamageFlag(EProjectileDamage::ManaDrain);
    const auto Hit = ResolvePlayerDamage(400, Mask, Typed.Player->GetResistanceMask(), 0, Typed.Player->GetArmorRating());
    Typed.Player->ReceiveTypedHit(400, Mask);
    TestEqual(TEXT("Ward charges full POST-mitigation health damage after ordinary typed mana loss"),
        Typed.Player->GetMana(), BeforeMana - Hit.Mana - Hit.Health);
    TestFalse(TEXT("The typed overkill player remains alive at zero"), Typed.Player->IsDefeated());
    TestEqual(TEXT("Independent worn passives are preserved"), Typed.Player->GetStatusPower(SpellShieldStatus), 5);
    const int32 PoisonMana = Typed.Player->GetMana();
    const auto PoisonHit = ResolvePlayerDamage(100, DamageFlag(EProjectileDamage::Poison), Typed.Player->GetResistanceMask(), 0, Typed.Player->GetArmorRating());
    Typed.Player->ReceiveTrapPoison(100);
    TestEqual(TEXT("Death Ward does not prevent ordinary poison accumulation"), Typed.Player->GetPoisonLevel(), PoisonHit.Poison);
    TestEqual(TEXT("A poison hit at zero still charges its full mitigated health amount"), Typed.Player->GetMana(), PoisonMana - PoisonHit.Health);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalDeathWardLifecycle, "AAOriginal.WAD.Gameplay.DeathWardLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalDeathWardLifecycle::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FDeathWardFixture Expiry(nullptr, TEXT(""), true);
    if (!Expiry.Valid() || !Expiry.Cast()) { return false; }
    Expiry.Player->ReceiveEncounterHit(100);
    const TArray<int32> Items = Expiry.Player->GetCarriedItems();
    Expiry.Player->Tick(30.1f);
    TestEqual(TEXT("Natural expiry removes Death Ward"), Expiry.Player->GetStatusPower(DeathWardStatus), 0);
    TestFalse(TEXT("Expiry itself does not proactively defeat an alive zero-health player"), Expiry.Player->IsDefeated());
    TestEqual(TEXT("Expiry does not raise health to one"), Expiry.Player->GetHealth(), 0);
    TestTrue(TEXT("Ward-supported survival and expiry do not drop inventory"), Expiry.Player->GetCarriedItems() == Items);
    Expiry.Player->ReceiveEncounterHit(1);
    TestTrue(TEXT("The next unwarded health-loss event causes true defeat"), Expiry.Player->IsDefeated());
    TestTrue(TEXT("True defeat disables character movement"), Expiry.Player->GetCharacterMovement()->MovementMode == MOVE_None);
    const int32 Mana = Expiry.Player->GetMana();
    Expiry.Player->ReceiveEncounterHit(100);
    TestEqual(TEXT("Further hits cannot repeat defeat or charge ward mana"), Expiry.Player->GetMana(), Mana);
    Expiry.Press(EKeys::SpaceBar);
    TestFalse(TEXT("Only the actual defeat state selects Space revival"), Expiry.Player->IsDefeated());
    TestEqual(TEXT("Revival keeps the established native full-health behavior"), Expiry.Player->GetHealth(), Expiry.Player->GetMaxHealth());

    FDeathWardFixture SavedDefeat(nullptr, TEXT("?WadHealth=500?WadDefeated=1"), true);
    if (!SavedDefeat.Valid()) { return false; }
    TestTrue(TEXT("Explicit defeat is independent of positive health too"), SavedDefeat.Player->IsDefeated());
    SavedDefeat.Player->RestoreWadVital(9, 100);
    TestEqual(TEXT("A scripted heal can change the statistic without reviving"), SavedDefeat.Player->GetHealth(), 600);
    TestTrue(TEXT("Positive health alone never clears the defeated state"), SavedDefeat.Player->IsDefeated());
    const int32 FailedCasts = SavedDefeat.Player->GetSuccessfulCasts();
    SavedDefeat.Cast();
    TestEqual(TEXT("A genuinely defeated player cannot cast despite positive health"), SavedDefeat.Player->GetSuccessfulCasts(), FailedCasts);
    SavedDefeat.Press(EKeys::SpaceBar);
    TestFalse(TEXT("Space uses explicit defeat for revival despite positive health"), SavedDefeat.Player->IsDefeated());

    FDeathWardFixture Dispel;
    if (!Dispel.Valid() || !Dispel.Cast()) { return false; }
    Dispel.Player->ReceiveEncounterHit(100);
    Dispel.Player->ReceiveTypedHit(1, SpecialDamageFlag(ESpecialDamage::Dispel));
    TestEqual(TEXT("Dispel removes the sole temporary source owner"), Dispel.Player->GetOwnedEffectCount(), 0);
    TestFalse(TEXT("Dispel at zero health is not itself a death event"), Dispel.Player->IsDefeated());
    Dispel.Player->RestoreWadVital(9, 250);
    TestEqual(TEXT("An alive zero-health character can heal after the ward has gone"), Dispel.Player->GetHealth(), 250);
    Dispel.Player->ReceiveEncounterHit(100);
    TestFalse(TEXT("Healing before the next hit avoids defeat normally"), Dispel.Player->IsDefeated());

    FDeathWardFixture Invulnerable;
    if (!Invulnerable.Valid() || !Invulnerable.Cast()) { return false; }
    Invulnerable.Player->ReceiveEncounterHit(100);
    Invulnerable.Player->Tick(1.0f);
    Invulnerable.Cast(TEXT("1547"));
    Invulnerable.Player->Tick(29.1f);
    TestEqual(TEXT("The earlier ward expired"), Invulnerable.Player->GetStatusPower(DeathWardStatus), 0);
    TestEqual(TEXT("The later invulnerability still protects"), Invulnerable.Player->GetStatusPower(InvulnerableStatus), 1);
    Invulnerable.Player->ReceiveTypedHit(100, NormalDamage);
    TestFalse(TEXT("Typed invulnerability stops before the raw health/death path"), Invulnerable.Player->IsDefeated());
    Invulnerable.Player->ReceiveEncounterHit(100);
    TestTrue(TEXT("Source raw health-loss still checks zero-health death after blocked subtraction"), Invulnerable.Player->IsDefeated());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalAliveZeroControls, "AAOriginal.WAD.Gameplay.AliveZeroControls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalAliveZeroControls::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FDeathWardFixture Fixture;
    if (!Fixture.Valid() || !Fixture.Cast()) { return false; }
    auto* Player = Fixture.Player;
    Player->ReceiveEncounterHit(100);
    const FVector Before = Player->GetActorLocation();
    const int32 Mana = Player->GetMana();
    // This isolated input fixture does not run a map's floor initialization.
    // Supply the normal grounded precondition; physical jump arcs are tested separately.
    Player->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    TestTrue(TEXT("The zero-health input fixture starts grounded"), Player->GetCharacterMovement()->IsMovingOnGround());
    Fixture.Press(EKeys::SpaceBar);
    TestEqual(TEXT("Space at alive zero health does not revive or refill stats"), Player->GetHealth(), 0);
    TestEqual(TEXT("Space at alive zero health preserves mana"), Player->GetMana(), Mana);
    TestTrue(TEXT("Alive-zero Space performs the normal jump instead"), Player->GetCharacterMovement()->Velocity.Z > 0.0 && Player->GetActorLocation().Equals(Before));
    ANativeWadGuardian* Enemy = Fixture.World->SpawnActor<ANativeWadGuardian>(FVector(0, 0, 32), FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("A real creature can assess the zero-health player"), Enemy)) { return false; }
    FWadEncounterProfile Profile;
    Profile.SourceType = 1018;
    Profile.MaxHealth = 700;
    Enemy->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
    Enemy->ResetForGenerator();
    Enemy->Tick(0.2f);
    TestTrue(TEXT("Enemy AI still acquires an alive zero-health player"), Enemy->HasAcquiredTarget());
    Player->CollectItem(833);
    if (!TestTrue(TEXT("Inventory take is allowed while alive at zero health"), Player->TakeInventoryItem(Player->GetCarriedItems().Num() - 1))
        || !TestTrue(TEXT("Normal ready-hand swap is allowed at zero health"), Player->SwapMouseAndReady())
        || !TestTrue(TEXT("Normal LeftControl use is available at zero health"), Fixture.Press(EKeys::LeftControl))) { return false; }
    TestEqual(TEXT("An ordinary created/source healing potion heals from zero"), Player->GetHealth(), 1000);
    TestFalse(TEXT("Healing never required a revival transition"), Player->IsDefeated());

    FDeathWardFixture Regeneration(nullptr, TEXT("?WadFood=2000?WadWater=2000"));
    if (!Regeneration.Valid() || !Regeneration.Cast()) { return false; }
    Regeneration.Player->ReceiveEncounterHit(100);
    const int32 Recovery = Regeneration.Player->GetHealthRegeneration();
    Regeneration.Player->Tick(6.0f);
    TestEqual(TEXT("Natural needs recovery is permitted by liveness, not positive health"), Regeneration.Player->GetHealth(), Recovery);
    TestFalse(TEXT("Natural recovery from zero remains alive"), Regeneration.Player->IsDefeated());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalDeathWardPersistence, "AAOriginal.WAD.Gameplay.DeathWardPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalDeathWardPersistence::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (const bool bExpireBeforeSave : {false, true})
    {
        FDeathWardFixture Fixture;
        if (!Fixture.Valid() || !Fixture.Cast()) { return false; }
        Fixture.Player->ReceiveEncounterHit(100);
        Fixture.Player->Tick(bExpireBeforeSave ? 30.1f : 1.25f);
        const FURL TownURL(nullptr, TEXT("/Game/Maps/WadTown?game=/Script/AAOriginal.NativeWadTownGameMode"), TRAVEL_Absolute);
        if (!TestTrue(TEXT("Use actual Town save eligibility at alive zero health"), Fixture.World->SetGameMode(TownURL))) { return false; }
        const FString Slot = TEXT("NativeDeathWardTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot, 0); };
        if (!TestTrue(TEXT("The actual checkpoint writer accepts a living zero-health player"), Fixture.Player->SaveWadCheckpoint(Slot))) { return false; }
        USaveGame* RawSave = UGameplayStatics::LoadGameFromSlot(Slot, 0);
        if (!TestNotNull(TEXT("Read the actual disk checkpoint"), RawSave)) { return false; }
        const auto* Save = static_cast<UNativeWadCheckpoint*>(RawSave);
        TestEqual(TEXT("The current checkpoint preserves liveness semantics"), Save->Version, UNativeWadCheckpoint::CurrentVersion);
        const FString Options = Save->LevelPackage + TEXT("?") + Save->PlayerOptions;
        const FURL URL(nullptr, *Options, TRAVEL_Absolute);
        TestEqual(TEXT("Modern travel options explicitly preserve alive state"), FString(URL.GetOption(TEXT("WadDefeated="), TEXT("missing"))), FString(TEXT("0")));
        TestEqual(TEXT("Modern travel options preserve actual zero health"), FString(URL.GetOption(TEXT("WadHealth="), TEXT("missing"))), FString(TEXT("0")));
        FDeathWardFixture Restored(*Options);
        if (!Restored.Valid()) { return false; }
        TestEqual(TEXT("A fresh actor restores zero rather than the old one-health clamp"), Restored.Player->GetHealth(), 0);
        TestFalse(TEXT("A fresh actor preserves modern alive-zero state even after the ward expired"), Restored.Player->IsDefeated());
        TestEqual(TEXT("Exact source-owner remaining time survives independently of life state"), Restored.Player->GetOwnedEffectRemaining(TEXT("spell:12")),
            Fixture.Player->GetOwnedEffectRemaining(TEXT("spell:12")));
        Restored.Player->ReceiveEncounterHit(1);
        TestEqual(TEXT("The first later hit follows restored ward presence, not a guessed liveness rule"), Restored.Player->IsDefeated(), bExpireBeforeSave);
        FURL Legacy(nullptr, *Options, TRAVEL_Absolute);
        Legacy.RemoveOption(TEXT("WadDefeated"));
        FDeathWardFixture Old(*Legacy.ToString());
        if (!Old.Valid()) { return false; }
        TestTrue(TEXT("A legacy zero-health save without the state bit migrates defeated"), Old.Player->IsDefeated());
        TestTrue(TEXT("Legacy defeat disables movement immediately"), Old.Player->GetCharacterMovement()->MovementMode == MOVE_None);
    }
    return true;
}
