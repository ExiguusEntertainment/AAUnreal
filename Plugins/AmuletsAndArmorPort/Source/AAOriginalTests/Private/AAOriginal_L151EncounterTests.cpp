#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    struct FL151Thing { int16 X, Y, Angle; uint16 Type, Data; };
    bool ReadOriginalL151(TArray<FL151Thing>& Things)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *(FPaths::ProjectDir() /
            TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L151.MAP"))) || Bytes.Num() < 12) { return false; }
        const auto U32 = [&](int32 At)
        { return uint32(Bytes[At]) | uint32(Bytes[At + 1]) << 8 | uint32(Bytes[At + 2]) << 16 | uint32(Bytes[At + 3]) << 24; };
        const uint32 Count = U32(4), Directory = U32(8);
        if (uint64(Directory) + uint64(Count) * 16 > uint64(Bytes.Num())) { return false; }
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            const int32 Entry = int32(Directory + Index * 16);
            if (FMemory::Memcmp(Bytes.GetData() + Entry + 8, "THINGS\0\0", 8)) { continue; }
            const uint32 Start = U32(Entry), Size = U32(Entry + 4);
            if (Size % 10 || uint64(Start) + Size > uint64(Bytes.Num())) { return false; }
            for (uint32 Position = Start; Position < Start + Size; Position += 10)
            {
                uint16 Values[5];
                for (int32 Field = 0; Field < 5; ++Field)
                { Values[Field] = uint16(Bytes[Position + Field * 2]) | uint16(Bytes[Position + Field * 2 + 1]) << 8; }
                Things.Add({int16(Values[0]), int16(Values[1]), int16(Values[2]), Values[3], Values[4]});
            }
            return true;
        }
        return false;
    }

    FName TagForThing(int32 Id)
    { return FName(*(Id == 31 ? FString(TEXT("L151 Gaiseric Magician")) : FString::Printf(TEXT("WAD Creature Thing %d"), Id))); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL151AuthoredEncounters, "AAOriginal.WAD.Import.L151AuthoredEncounters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalL151AuthoredEncounters::RunTest(const FString& Parameters)
{
    TArray<FL151Thing> Things;
    if (!TestTrue(TEXT("Read original binary THINGS independently of the native loader/importer"), ReadOriginalL151(Things))) { return false; }
    int32 Black = 0, White = 0;
    for (const auto& Thing : Things) { Black += Thing.Type == 3001; White += Thing.Type == 3002; }
    TestEqual(TEXT("Original L151 contains eleven exact Black Barbarians"), Black, 11);
    TestEqual(TEXT("Original L151 contains nine exact White Barbarians, including the old slice"), White, 9);
    if (!TestTrue(TEXT("Original Thing31 remains White Barbarian3002"), Things.IsValidIndex(31) && Things[31].Type == 3002)) { return false; }
    auto* World = LoadObject<UWorld>(nullptr, TEXT("/Game/Maps/L151.L151"));
    if (!TestNotNull(TEXT("Saved L151 content after the bounded encounter refresh"), World)) { return false; }
    TMap<FName, ANativeWadGuardian*> Actors;
    int32 Actual = 0;
    for (TActorIterator<ANativeWadGuardian> Enemy(World); Enemy; ++Enemy)
    {
        const int32 Type = Enemy->GetSourceType();
        if (Type != 3001 && Type != 3002) { continue; }
        ++Actual;
        if (!TestTrue(TEXT("Every authored encounter has a primary checkpoint identity"), !Enemy->Tags.IsEmpty())) { return false; }
        TestFalse(TEXT("No two authored actors share a primary identity"), Actors.Contains(Enemy->Tags[0]));
        Actors.Add(Enemy->Tags[0], *Enemy);
    }
    TestEqual(TEXT("Saved map contains all20 original combat actors, not only the old slice"), Actual, Black + White);
    for (int32 Id = 0; Id < Things.Num(); ++Id)
    {
        const auto& Thing = Things[Id];
        if (Thing.Type != 3001 && Thing.Type != 3002) { continue; }
        auto* Enemy = Actors.FindRef(TagForThing(Id));
        if (!TestNotNull(*FString::Printf(TEXT("Exact source Thing%d has its own actor"), Id), Enemy)) { return false; }
        const auto& Profile = Enemy->GetEncounterProfile();
        TestEqual(TEXT("Full source creature type survives import"), Profile.SourceType, int32(Thing.Type));
        TestEqual(TEXT("Authored placement X is retained"), Enemy->GetActorLocation().X, double(Thing.X));
        TestEqual(TEXT("Authored placement Y is retained"), Enemy->GetActorLocation().Y, double(Thing.Y));
        TestEqual(TEXT("Original family max health is not inherited from the old Elf Guardian default"), Profile.MaxHealth, Thing.Type == 3001 ? 3000 : 2500);
        TestTrue(TEXT("The ordinary original sprite is saved"), Enemy->GetPortraitMaterial() != nullptr);
        TestEqual(TEXT("Black chain versus White cloth is retained"), Profile.ArmorClass, Thing.Type == 3001 ? 1 : 0);
        TestEqual(TEXT("Source attack continuation is retained"), Profile.ChanceContinueAttack, 75);
        if (Id == 31)
        {
            TestEqual(TEXT("Existing old slice checkpoint identity stays first"), Enemy->Tags[0], FName(TEXT("L151 Gaiseric Magician")));
            TestEqual(TEXT("The old slice's visible name remains recognizable"), Enemy->GetEncounterName(), FString(TEXT("Gaiseric Magician")));
        }
    }
    for (TActorIterator<ANativeWadGuardian> Enemy(World); Enemy; ++Enemy)
    { TestFalse(TEXT("The old slice has not been duplicated under a generic Thing31 identity"), Enemy->ActorHasTag(TEXT("WAD Creature Thing 31"))); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL151CombatFamilies, "AAOriginal.WAD.Gameplay.L151CombatFamilies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalL151CombatFamilies::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!World) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    for (const int32 Type : {3001, 3002})
    {
        auto* Creature = World->SpawnActor<ANativeWadGuardian>();
        if (!Creature) { return false; }
        FWadEncounterProfile Source;
        Source.SourceType = Type;
        Creature->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Source);
        Creature->DispatchBeginPlay();
        const auto& Profile = Creature->GetEncounterProfile();
        TestEqual(TEXT("The runtime starts the actual source family health"), Creature->GetEncounterHealth(), Type == 3001 ? 3000 : 2500);
        TestEqual(TEXT("Source melee power is family-specific"), Profile.MeleeDamage, Type == 3001 ? 400 : 250);
        TestEqual(TEXT("Source hurt/retreat health is family-specific"), Profile.RetreatHealth, Type == 3001 ? 500 : 1000);
        TestEqual(TEXT("Source magic dart maximum range remains the original unusual Black5000/White10000"), Profile.BoltRange, Type == 3001 ? 5000 : 10000);
        TestEqual(TEXT("Source magic dart minimum range is retained"), Profile.MinBoltRange, Type == 3001 ? 5000.0f : 250.0f);
        TestEqual(TEXT("Source ranged delay is retained"), Profile.RangedDelayUpdates, Type == 3001 ? 50 : 25);
        TestEqual(TEXT("Source update cadence is retained"), Profile.UpdateTicks, Type == 3001 ? 7 : 5);
        TestEqual(TEXT("Source one-point regeneration is retained"), Profile.RegenPerUpdate, 1);
        TestEqual(TEXT("Source75% Berserk creature retarget chance is retained"), Profile.ChanceAttackCreature, 75);
        TestFalse(TEXT("Both source families stay grounded"), Profile.bCanFly);
        const int32 Before = Creature->GetEncounterHealth();
        if (!TestTrue(TEXT("An actual electric hit reaches the family's native damage resolver"), Creature->TakeHit(100, nullptr, DamageFlag(EProjectileDamage::Electricity)))) { return false; }
        TestEqual(TEXT("Source chain increases electric injury by25%; cloth does not"), Creature->GetEncounterHealth(), Before - (Type == 3001 ? 125 : 100));
        const int32 Life = Creature->GetCreatureLife();
        Creature->RestoreEncounterHealth(0);
        Creature->ResetForGenerator();
        TestEqual(TEXT("Reuse restores this family's maximum health"), Creature->GetEncounterHealth(), Profile.MaxHealth);
        TestEqual(TEXT("Reuse creates a new life without creating a new actor"), Creature->GetCreatureLife(), Life + 1);
        TestEqual(TEXT("Reuse preserves the original full source type"), Creature->GetSourceType(), Type);
        Creature->Destroy();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalL151CheckpointIdentity, "AAOriginal.WAD.Gameplay.L151CheckpointIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalL151CheckpointIdentity::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!World) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Level = World->SpawnActor<ANativeWadLevel>();
    if (!Level) { return false; }
    TArray<ANativeWadGuardian*> Creatures;
    const int32 Types[] = {3002, 3001};
    for (int32 Index = 0; Index < 2; ++Index)
    {
        auto* Creature = World->SpawnActor<ANativeWadGuardian>(FVector(Index * 200, 0, 32), FRotator::ZeroRotator);
        if (!Creature) { return false; }
        FWadEncounterProfile Source;
        Source.SourceType = Types[Index];
        Creature->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Source);
        Creature->Tags.Add(Index == 0 ? FName(TEXT("L151 Gaiseric Magician")) : FName(TEXT("WAD Creature Thing 25")));
        Creature->RestoreEncounterHealth(Index == 0 ? 1700 : 2200);
        Creature->RestoreCreatureLife(4 + Index);
        Creatures.Add(Creature);
    }
    FWadLevelCheckpoint Saved;
    if (!TestTrue(TEXT("Both families write through the real level checkpoint snapshot"), Level->WriteCheckpoint(Saved))) { return false; }
    TestTrue(TEXT("The legacy old-save health key is retained"), Saved.Encounters.Contains(TEXT("L151 Gaiseric Magician")));
    TestEqual(TEXT("Every distinct authored life gets its own saved identity"), Saved.Encounters.Num(), 2);
    for (auto* Creature : Creatures) { Creature->ResetForGenerator(); Creature->RestoreEncounterHealth(1); }
    if (!TestTrue(TEXT("The real checkpoint restore resolves existing actor identities"), Level->RestoreCheckpoint(Saved))) { return false; }
    for (int32 Index = 0; Index < Creatures.Num(); ++Index)
    {
        TestEqual(TEXT("Checkpoint health returns to the same existing actor"), Creatures[Index]->GetEncounterHealth(), Index == 0 ? 1700 : 2200);
        TestEqual(TEXT("The saved life identity is restored rather than guessed from health"), Creatures[Index]->GetCreatureLife(), 4 + Index);
        TestEqual(TEXT("Source combat identity is independent of saved legacy tag spelling"), Creatures[Index]->GetSourceType(), Types[Index]);
    }
    // A pre-restoration L151 checkpoint knew only Thing31. Missing newly imported
    // encounter entries remain ordinary live actors; the old actor health key still matches.
    Saved.Encounters.Remove(TEXT("WAD Creature Thing 25"));
    Creatures[0]->RestoreEncounterHealth(1);
    Creatures[1]->RestoreEncounterHealth(2300);
    if (!Level->RestoreCheckpoint(Saved)) { return false; }
    TestEqual(TEXT("Old L151 single-actor save still restores the preserved slice identity"), Creatures[0]->GetEncounterHealth(), 1700);
    TestEqual(TEXT("Old saves do not accidentally overwrite newly authored encounters"), Creatures[1]->GetEncounterHealth(), 2300);
    int32 Count = 0;
    for (TActorIterator<ANativeWadGuardian> It(World); It; ++It) { ++Count; }
    TestEqual(TEXT("Checkpoint/reuse never duplicates either actor"), Count, 2);
    return true;
}
