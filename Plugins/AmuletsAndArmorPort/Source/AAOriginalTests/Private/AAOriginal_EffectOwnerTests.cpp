#include "Misc/AutomationTest.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/World.h"
#include "NativeWadPlayerEffects.h"
#include "NativeWadRules.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalEffectOwners, "AAOriginal.WAD.Gameplay.EffectOwners",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalEffectOwners::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    FWadPlayerEffects Effects;
    const auto Item = [&](int32 Type, int32 Power, double Seconds)
    { return Effects.Apply({FWadPlayerEffects::ConsumableOwner(Type), EWadTimedEffectKind::Status, HealthRegenStatus, Power, Seconds}); };
    TestTrue(TEXT("The original weaker regeneration potion creates a temporary owner"), Item(803, 300, 40.0));
    Effects.Tick(5.0);
    TestEqual(TEXT("The owner's time decreases"), Effects.Remaining(TEXT("item:803")), 35.0);
    Item(803, 300, 40.0);
    TestEqual(TEXT("Using the same potion type refreshes one source owner"), Effects.Num(), 1);
    TestEqual(TEXT("The refreshed source type has its new full duration"), Effects.Remaining(TEXT("item:803")), 40.0);
    Item(804, 600, 57.0);
    TestEqual(TEXT("Different potion types with one status have different source owners"), Effects.Num(), 2);
    TestEqual(TEXT("Their source regeneration powers add"), Effects.Power(EWadTimedEffectKind::Status, HealthRegenStatus), 900);
    Effects.Tick(41.0);
    TestEqual(TEXT("Each owner expires independently"), Effects.Num(), 1);
    TestEqual(TEXT("The stronger potion survives the earlier owner's expiry"), Effects.Power(EWadTimedEffectKind::Status, HealthRegenStatus), 600);
    Effects.Apply({FWadPlayerEffects::SpellOwner(21), EWadTimedEffectKind::Status, HealthRegenStatus, 500, 60.0});
    Effects.Apply({FWadPlayerEffects::SpellOwner(21), EWadTimedEffectKind::Status, HealthRegenStatus, 500, 60.0});
    TestEqual(TEXT("Recasting the same source spell refreshes its own record"), Effects.Num(), 2);
    TestEqual(TEXT("A spell and a different consumable source coexist"), Effects.Power(EWadTimedEffectKind::Status, HealthRegenStatus), 1100);
    TestTrue(TEXT("Dispel selects one owner, not an entire status type"), Effects.Dispel(0));
    TestEqual(TEXT("The other owner survives Dispel"), Effects.Power(EWadTimedEffectKind::Status, HealthRegenStatus), 500);
    TestEqual(TEXT("Only the selected owner was removed"), Effects.Num(), 1);
    TestFalse(TEXT("An invalid Dispel index changes no state"), Effects.Dispel(10));

    Effects.Reset();
    Effects.Apply({FWadPlayerEffects::SpellOwner(24), EWadTimedEffectKind::Status, SpeedStatus, 10, 120.0});
    Effects.Apply({FWadPlayerEffects::SpellOwner(32), EWadTimedEffectKind::Status, SpeedStatus, 15, 120.0});
    TestEqual(TEXT("Two distinct Mage speed definitions stack before attribute clamping"), Effects.Power(EWadTimedEffectKind::Status, SpeedStatus), 25);
    Effects.Apply({FWadPlayerEffects::IncomingControlOwner, EWadTimedEffectKind::Status, SpeedStatus, -20, 30.0});
    TestEqual(TEXT("An incoming Slow adds its signed modifier alongside other owners"), Effects.Power(EWadTimedEffectKind::Status, SpeedStatus), 5);
    Effects.Apply({FWadPlayerEffects::IncomingControlOwner, EWadTimedEffectKind::Paralysis, INDEX_NONE, 1, 10.0});
    TestEqual(TEXT("Paralyze replaces the source shared Slow owner"), Effects.Power(EWadTimedEffectKind::Status, SpeedStatus), 25);
    TestEqual(TEXT("Paralyze retains its established native duration"), Effects.Duration(EWadTimedEffectKind::Paralysis), 10.0);
    Effects.Apply({FWadPlayerEffects::IncomingControlOwner, EWadTimedEffectKind::Status, SpeedStatus, -20, 30.0});
    TestEqual(TEXT("A new Slow replaces that same Paralyze owner"), Effects.Duration(EWadTimedEffectKind::Paralysis), 0.0);
    TestEqual(TEXT("Other speed owners were not erased by replacement"), Effects.Num(), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalEffectOwnerPersistence, "AAOriginal.WAD.Gameplay.EffectOwnerPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalEffectOwnerPersistence::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const FWadTimedPlayerEffect LegacyEntry{FWadPlayerEffects::LegacyOwner(EWadTimedEffectKind::Status, SpeedStatus),
        EWadTimedEffectKind::Status, SpeedStatus, -20, 12.25};
    const TArray<FWadTimedPlayerEffect> Legacy{LegacyEntry};
    FWadPlayerEffects Effects;
    TestTrue(TEXT("Missing modern state migrates an old aggregate timer"), Effects.Restore(nullptr, Legacy));
    TestEqual(TEXT("The unknowable original owner stays explicitly Legacy"), Effects.Remaining(LegacyEntry.Owner), 12.25);
    Effects.Apply({FWadPlayerEffects::ConsumableOwner(803), EWadTimedEffectKind::Status, HealthRegenStatus, 300, 20.125});
    Effects.Apply({FWadPlayerEffects::SpellOwner(31), EWadTimedEffectKind::Status, ManaRegenStatus, 500, 70.01});
    Effects.Apply({FWadPlayerEffects::SpellOwner(41), EWadTimedEffectKind::Flight, INDEX_NONE, 1, 29.75});
    Effects.Apply({FWadPlayerEffects::ConsumableOwner(808), EWadTimedEffectKind::ShieldPotion, 0, 10, 140.0});
    const FString Encoded = Effects.Encode();
    const FURL URL(nullptr, *(TEXT("/Game/Maps/L30?WadOwnedEffects=") + Encoded + TEXT("?WadFood=900")), TRAVEL_Absolute);
    TestEqual(TEXT("The payload survives Unreal's actual travel-option parser"), FString(URL.GetOption(TEXT("WadOwnedEffects="), TEXT("missing"))), Encoded);
    FWadPlayerEffects Reloaded;
    TestTrue(TEXT("The versioned registry reopens"), Reloaded.Decode(Encoded));
    TestEqual(TEXT("All distinct owners survive serialization"), Reloaded.Num(), 5);
    TestEqual(TEXT("Source consumable identity and exact remaining time survive"), Reloaded.Remaining(TEXT("item:803")), 20.125);
    TestEqual(TEXT("A regeneration power above the old hundred-point bound survives"), Reloaded.Power(EWadTimedEffectKind::Status, ManaRegenStatus), 500);
    TestEqual(TEXT("Legacy ownership is retained after re-saving, not guessed again"), Reloaded.Remaining(LegacyEntry.Owner), 12.25);
    Reloaded.Tick(13.0);
    TestEqual(TEXT("A restored legacy owner expires normally"), Reloaded.Remaining(LegacyEntry.Owner), 0.0);
    TestTrue(TEXT("An explicitly empty modern registry is valid"), Reloaded.Restore(TEXT("1"), Legacy));
    TestEqual(TEXT("Empty modern state overrides populated legacy fields"), Reloaded.Num(), 0);

    Reloaded.Apply({TEXT("item:803"), EWadTimedEffectKind::Status, HealthRegenStatus, 300, 20.0});
    for (const TCHAR* Bad : {TEXT("2"), TEXT("1~item:803,0,21,300,nan"), TEXT("1~item:803,0,21,300,-1"),
        TEXT("1~item:803,0,21,300,3~item:803,0,21,300,4"), TEXT("1~item:803,99,0,1,30"), TEXT("1~bad?owner,0,1,1,30"),
        TEXT("1~item:803,0junk,21,300,3"), TEXT("1~item:803,0,21junk,300,3"),
        TEXT("1~item:803,0,21,300junk,3"), TEXT("1~item:803,0,21,300,3seconds"),
        TEXT("1~item:803,4294967296,21,300,3"), TEXT("1~item:803,0,4294967317,300,3"),
        TEXT("1~item:803,0,21,4294967596,3"), TEXT("1~item:803,0,21,-4294967596,3"),
        TEXT("1~item:803,0,21,9999999999999999999999999999999999999999,3"),
        TEXT("1~item:803,0,21,300,inf"), TEXT("1~item:803,0,21,300,NaN"),
        TEXT("1~item:803,0,21,300,1e99999"), TEXT("1~item:803,0,21,300,1e-99999"),
        TEXT("1~item:803,0,21,300,1e"), TEXT("1~item:803,0,21,300,1e+"),
        TEXT("1~item:803,0,21,300,3.0.0"), TEXT("1~item:803,0,21,300, 3"),
        TEXT("1~item:803,0,21,300,3 "), TEXT("1~item:803,0,21,300,0x1p1")})
    {
        TestFalse(TEXT("Malformed, future, or duplicate-owner state is refused"), Reloaded.Decode(Bad));
        TestEqual(TEXT("Refusing an invalid payload leaves the previous valid state intact"), Reloaded.Num(), 1);
        TestEqual(TEXT("Invalid state cannot change the previous owner's power"), Reloaded.Power(EWadTimedEffectKind::Status, HealthRegenStatus), 300);
    }
    for (const double TinySeconds : {1.0e-12, std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min()})
    {
        FWadPlayerEffects Tiny;
        TestTrue(TEXT("An unexpired tiny positive duration remains valid"),
            Tiny.Apply({TEXT("item:803"), EWadTimedEffectKind::Status, HealthRegenStatus, 300, TinySeconds}));
        FWadPlayerEffects TinyReloaded;
        TestTrue(TEXT("Scientific-notation durations reopen without rounding to zero"), TinyReloaded.Decode(Tiny.Encode()));
        TestEqual(TEXT("Tiny positive durations round-trip exactly"), TinyReloaded.Remaining(TEXT("item:803")), TinySeconds);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSourceStatusSpells, "AAOriginal.WAD.Gameplay.SourceStatusSpells",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalSourceStatusSpells::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    const int32 Definitions[] = {16,21,23,24,25,26,27,30,31,32,35,38,40,55,56,59,60,61,63,65,66,67,81,83,84,88,89,92,93,94,95,96,97};
    const TMap<FString, int32> Statuses{
        {TEXT("STEALTH MOD"), StealthStatus}, {TEXT("HEALTH REGEN MOD"), HealthRegenStatus}, {TEXT("MANA REGEN MOD"), ManaRegenStatus},
        {TEXT("STRENGTH MOD"), StrengthStatus}, {TEXT("SPEED MOD"), SpeedStatus}, {TEXT("MAGIC MOD"), MagicStatus},
        {TEXT("ACCURACY MOD"), AccuracyStatus}, {TEXT("TRANSLUCENT"), TranslucentStatus}, {TEXT("INVISIBLE"), InvisibleStatus},
        {TEXT("ELECTRICITY ATTACK"), ElectricityAttackStatus}, {TEXT("FIRE ATTACK"), FireAttackStatus},
        {TEXT("ACID ATTACK"), AcidAttackStatus}, {TEXT("POISON ATTACK"), PoisonAttackStatus},
        {TEXT("PIERCING ATTACK"), PiercingAttackStatus}, {TEXT("MANA DRAIN ATTACK"), ManaDrainAttackStatus},
        {TEXT("LAVA WALK"), LavaWalkStatus}, {TEXT("RESIST POISON"), PoisonResistStatus}, {TEXT("RESIST FIRE"), FireResistStatus},
        {TEXT("RESIST ACID"), AcidResistStatus}, {TEXT("RESIST ELECTRICITY"), ElectricityResistStatus}, {TEXT("RESIST PIERCING"), PiercingResistStatus}};
    for (const int32 Definition : Definitions)
    {
        const FWadSpell* Spell = Algo::FindByPredicate(SupportedSpells, [&](const auto& Entry)
            { return Entry.SourceDefinition == Definition && Entry.Effect == ESpellEffect::Status; });
        if (!TestNotNull(TEXT("Each audited source status spell has a native definition"), Spell)) { return false; }
        TArray<FString> Input;
        const FString Path = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal/SPELDESC")
            / FString::Printf(TEXT("SPL%05d.TXT"), Definition);
        if (!TestTrue(TEXT("The actual source spell definition is readable"), FFileHelper::LoadFileToStringArray(Input, *Path))) { return false; }
        TArray<FString> Lines;
        for (const auto& Line : Input)
        { const FString Trimmed = Line.TrimStartAndEnd(); if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("#"))) { Lines.Add(Trimmed); } }
        const auto Field = [&](const TCHAR* Name)
        { const int32 Index = Lines.Find(Name); return Lines.IsValidIndex(Index + 1) && Index != INDEX_NONE ? Lines[Index + 1] : FString(); };
        FString Runes = Field(TEXT("ACCESS CODE"));
        Runes.ReplaceInline(TEXT(","), TEXT(""));
        Runes.ReplaceInline(TEXT("0"), TEXT(""));
        Runes.ReplaceInline(TEXT(" "), TEXT(""));
        TestEqual(TEXT("Rune sequence matches the source"), FString(Spell->Runes), Runes);
        TestEqual(TEXT("Mana cost matches the source"), Spell->Mana, FCString::Atoi(*Field(TEXT("SPELL COST"))));
        TestEqual(TEXT("Casting hardness matches the source"), Spell->Hardness, FCString::Atoi(*Field(TEXT("SPELL HARDNESS"))));
        TestEqual(TEXT("Duration matches source ticks"), Spell->DurationTicks, FCString::Atoi(*Field(TEXT("SPELL DURATION"))));
        TestEqual(TEXT("Power matches the source"), Spell->Power, FCString::Atoi(*Field(TEXT("SPELL POWER"))));
        const FString School = Spell->School == ESpellSchool::Mage ? TEXT("MAGE") : Spell->School == ESpellSchool::Cleric ? TEXT("CLERIC") : TEXT("ARCANE");
        TestEqual(TEXT("Spell school matches the source"), School, Field(TEXT("SPELL SYSTEM")));
        TestTrue(TEXT("The native status is one of the supported effect kinds"), Spell->Status >= 0 && Spell->Status < ItemStatusCount);
        const int32* ExpectedStatus = Statuses.Find(Field(TEXT("EFFECT SUBTYPE")));
        if (!TestNotNull(TEXT("The source subtype has an audited native effect mapping"), ExpectedStatus)) { return false; }
        TestEqual(TEXT("The spell applies the source subtype, not only its numeric metadata"), Spell->Status, *ExpectedStatus);
    }
    return true;
}
