#include "Misc/AutomationTest.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr int32 OrdinarySources[] = {1002, 1008, 1009, 1011, 1014, 1018, 1019,
        1032, 1035, 1036, 3000, 3001, 3002, 3003, 4080, 50158};

    TArray<ANativeWadPickup*> OrdinaryDrops(UWorld& World)
    {
        TArray<ANativeWadPickup*> Result;
        for (TActorIterator<ANativeWadPickup> Item(&World); Item; ++Item)
        {
            if (Item->IsDropped() && !Item->IsActorBeingDestroyed()) { Result.Add(*Item); }
        }
        return Result;
    }

    bool InstallTreasureFloor(ANativeWadLevel& Level)
    {
        const FArrayProperty* Property = FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(), TEXT("Sectors"));
        if (!Property) { return false; }
        auto& Sectors = *Property->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(&Level);
        FAAWadNativeSector& Sector = Sectors.AddDefaulted_GetRef();
        Sector.RepairedBoundaryPoints = {{-500, -500}, {500, -500}, {500, -500}, {500, 500},
            {500, 500}, {-500, 500}, {-500, 500}, {-500, -500}};
        Sector.Floor = NewObject<UStaticMeshComponent>(&Level);
        Sector.Ceiling = NewObject<UStaticMeshComponent>(&Level);
        Sector.Floor->SetupAttachment(Level.GetRootComponent());
        Sector.Ceiling->SetupAttachment(Level.GetRootComponent());
        Sector.Ceiling->SetRelativeLocation(FVector(0, 0, 256));
        Sector.Floor->RegisterComponent();
        Sector.Ceiling->RegisterComponent();
        Level.SetSectorFloorZ(0, 0);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalOrdinaryTreasureTables,
    "AAOriginal.WAD.Gameplay.OrdinaryTreasureTables",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalOrdinaryTreasureTables::RunTest(const FString& Parameters)
{
    FString Source;
    const FString Path = FPaths::ProjectDir() /
        TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Source/CRELOGIC.C");
    if (!TestTrue(TEXT("Read the actual original treasure implementation"), FFileHelper::LoadFileToString(Source, *Path))) { return false; }
    const auto ReadTable = [&](const TCHAR* Name, TArray<int32>& Table)
    {
        const FRegexPattern Pattern(FString::Printf(TEXT("%s\\[12\\]\\s*=\\s*\\{([^}]+)\\}"), Name));
        FRegexMatcher Match(Pattern, Source);
        if (!Match.FindNext()) { return false; }
        TArray<FString> Fields;
        Match.GetCaptureGroup(1).ParseIntoArray(Fields, TEXT(","));
        for (const FString& Field : Fields)
        {
            int32 Type = 0;
            if (!LexTryParseString(Type, *Field.TrimStartAndEnd()) || Type < 1 || Type > MAX_uint16) { return false; }
            Table.Add(Type);
        }
        return Table.Num() == 12;
    };
    TArray<int32> BadKnight, EvilKnight, Trojan;
    if (!TestTrue(TEXT("Read the original Bad Knight table"), ReadTable(TEXT("badKnightList"), BadKnight))
        || !TestTrue(TEXT("Read the original Evil Knight table"), ReadTable(TEXT("evilKnightList"), EvilKnight))
        || !TestTrue(TEXT("Read the original Trojan table"), ReadTable(TEXT("trojanList"), Trojan))) { return false; }
    // Exhaust the helper's complete 16-bit input domain. This verifies each
    // branch and modulo position, not the distribution/sequence of source RandomValue().
    for (int32 Input = 0; Input <= MAX_uint16; ++Input)
    {
        const uint16 Roll = uint16(Input);
        const auto Exact = [&](int32 Type, int32 Item, int32 Ammo = INDEX_NONE, int32 Count = 0)
        {
            const FWadCreatureTreasure Result = ResolveWadCreatureTreasure(Type, Roll);
            if (Result.ItemType == Item && Result.AmmoIndex == Ammo && Result.AmmoQuantity == Count) { return true; }
            AddError(FString::Printf(TEXT("Source %d, input %d: got item/ammo/quantity %d/%d/%d, expected %d/%d/%d"),
                Type, Input, Result.ItemType, Result.AmmoIndex, Result.AmmoQuantity, Item, Ammo, Count));
            return false;
        };
        if (!Exact(1002, (Roll & 1) ? 102 : 362) || !Exact(1008, 147, 2, 6) || !Exact(1009, 144, 0, 12)
            || !Exact(1011, Trojan[Roll % 12]) || !Exact(1014, 145, 3, 6)
            || !Exact(1032, 416) || !Exact(1035, BadKnight[Roll % 12]) || !Exact(1036, 106)
            || !Exact(3000, 101) || !Exact(3001, 102) || !Exact(3002, 102) || !Exact(3003, 101)
            || !Exact(4080, EvilKnight[Roll % 12]) || !Exact(50158, 417)) { return false; }
    }
    for (int32 Type : {0, 1006, 1012, 1015, 1037, 16368, 24560, 33803, 37866})
    {
        TestEqual(TEXT("Unimplemented journals/packed families and source no-drop cases gain no guessed treasure"),
            ResolveWadCreatureTreasure(Type, 1).ItemType, 0);
    }
    TestTrue(TEXT("Elymas's full identity does not collapse to the Orange Wizard journal table"),
        ResolveWadCreatureTreasure(50158, 1).ItemType == 417 && ResolveWadCreatureTreasure(50158 & 0xfff, 1).ItemType == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalOrdinaryTreasureLives,
    "AAOriginal.WAD.Gameplay.OrdinaryTreasureLives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalOrdinaryTreasureLives::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("World for original defeat loot"), World)) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ANativeWadLevel* Level = World->SpawnActor<ANativeWadLevel>();
    if (!TestNotNull(TEXT("Native level for pickup attachment and persistence"), Level)
        || !TestTrue(TEXT("Install one usable floor without importing geometry"), InstallTreasureFloor(*Level))) { return false; }
    for (const int32 Type : OrdinarySources)
    {
        ANativeWadGuardian* Creature = World->SpawnActor<ANativeWadGuardian>(FVector(0, 0, 34), FRotator::ZeroRotator);
        if (!Creature) { return false; }
        Creature->Tags.Add(FName(*FString::Printf(TEXT("OrdinaryTreasureLife%d"), Type)));
        FWadEncounterProfile Profile;
        Profile.SourceType = Type;
        Profile.MaxHealth = 2000;
        Profile.CollisionHalfHeight = 32;
        // Exercise the exact legacy boss profiles still used by the map importer.
        if (Type == 1032 || Type == 50158) { Profile.SourceType = 0; Profile.Name = Type == 1032 ? TEXT("Exiguus") : TEXT("Elymas"); }
        if (Type == 1008 || Type == 1009)
        {
            Profile.SourceType = 0;
            Profile.Name = Type == 1008 ? TEXT("Mounted Knight of Andrew") : TEXT("Knight of Andrew Crossbowman");
        }
        if (Type >= 3000 && Type <= 3003)
        {
            const TCHAR* Names[] = {TEXT("Bronze Barbarian"), TEXT("Black Barbarian"), TEXT("White Barbarian"), TEXT("Blue Barbarian")};
            Profile.SourceType = 0;
            Profile.Name = Names[Type - 3000];
        }
        Creature->Configure(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, Profile);
        Creature->RestoreEncounterHealth(Profile.MaxHealth);
        // Eastern Archers resist normal/fire damage. Defeat them with their
        // legitimate electricity weakness instead of bypassing source resistances.
        const uint8 DefeatDamage = Type == 1014 ? NativeWadRules::DamageFlag(NativeWadRules::EProjectileDamage::Electricity)
            : NativeWadRules::NormalDamage;
        ANativeWadPickup* QuestDrop = nullptr;
        if (Type == 1032 || Type == 50158)
        {
            QuestDrop = World->SpawnActor<ANativeWadPickup>();
            if (!QuestDrop) { return false; }
            QuestDrop->Tags.Add(FName(*FString::Printf(TEXT("OrdinaryTreasureQuest%d"), Type)));
            QuestDrop->Configure(nullptr, nullptr, Type == 1032 ? NativeWadRules::OrbOfTheUndead : NativeWadRules::ElymasNotes);
            Creature->SetDefeatPickup(QuestDrop);
            TestFalse(TEXT("The separate authored quest object stays hidden before death"), QuestDrop->IsAvailable());
        }
        if (Type == 1014)
        {
            TestFalse(TEXT("The source Eastern Archer refuses an entirely normal hit"), Creature->TakeHit(Profile.MaxHealth));
            TestEqual(TEXT("The refused normal hit leaves the Eastern Archer alive at full health"),
                Creature->GetEncounterHealth(), Profile.MaxHealth);
            TestFalse(TEXT("An immune hit is not a creature defeat"), Creature->IsDefeated());
            TestTrue(TEXT("An immune hit cannot release ordinary treasure"), OrdinaryDrops(*World).IsEmpty());
        }
        if (!TestTrue(TEXT("The fixture's damage is accepted by the source resistance rules"),
                Creature->TakeHit(Profile.MaxHealth, nullptr, DefeatDamage))
            || !TestTrue(TEXT("A real creature defeat precedes the treasure assertions"), Creature->IsDefeated())) { return false; }
        auto Drops = OrdinaryDrops(*World);
        if (!TestEqual(FString::Printf(TEXT("Source %d releases exactly one additional ordinary item"), Type), Drops.Num(), 1)) { return false; }
        TSet<int32> Choices;
        for (uint16 Roll = 0; Roll < 12; ++Roll) { Choices.Add(ResolveWadCreatureTreasure(Type, Roll).ItemType); }
        TestTrue(TEXT("Actual defeat selects a member of the original table"), Choices.Contains(Drops[0]->GetItemType()));
        for (uint16 Roll = 0; Roll < 12; ++Roll)
        {
            const FWadCreatureTreasure Expected = ResolveWadCreatureTreasure(Type, Roll);
            if (Expected.ItemType == Drops[0]->GetItemType())
            {
                TestEqual(TEXT("Actual defeat retains the source ammo family"), Drops[0]->GetAmmoIndex(), Expected.AmmoIndex);
                TestEqual(TEXT("Actual defeat retains the source custom quantity"), Drops[0]->GetAmmoQuantity(), Expected.AmmoQuantity);
                break;
            }
        }
        const UStaticMeshComponent* Art = Drops[0]->FindComponentByClass<UStaticMeshComponent>();
        TestTrue(TEXT("Every defeat branch has real saved world art"), Art && Art->GetMaterial(0));
        TestTrue(TEXT("The ordinary drop retains bottom rooting"), Art
            && FMath::IsNearlyEqual(Drops[0]->GetActorLocation().Z - Art->GetRelativeScale3D().Y * 50.0, 2.0, 0.01));
        if (QuestDrop)
        {
            TestTrue(TEXT("The existing quest drop becomes available alongside its extra ring"), QuestDrop->IsAvailable());
            TestEqual(TEXT("A boss drops only its additional ring through ordinary loot"), Drops[0]->GetItemType(), Type == 1032 ? 416 : 417);
        }
        Creature->TakeHit(1000);
        Creature->RestoreEncounterHealth(0);
        TestEqual(TEXT("Finishing hits and defeated-state restoration do not duplicate loot"), OrdinaryDrops(*World).Num(), 1);
        FWadLevelCheckpoint Saved;
        if (!TestTrue(TEXT("Archive the actual ordinary pickup and defeated actor"), Level->WriteCheckpoint(Saved))) { return false; }
        if (!TestEqual(TEXT("Exactly one ordinary drop is archived"), Saved.DroppedItems.Num(), 1)) { return false; }
        const FWadSavedDrop Original = Saved.DroppedItems[0];
        Creature->ResetForGenerator();
        if (!TestTrue(TEXT("Reopen the real checkpoint"), Level->RestoreCheckpoint(Saved))) { return false; }
        Drops = OrdinaryDrops(*World);
        if (!TestEqual(TEXT("Checkpoint replacement retains exactly one ordinary drop"), Drops.Num(), 1)) { return false; }
        TestTrue(TEXT("A restored corpse cannot roll its table again"), Creature->IsDefeated() && Creature->HasHandledDefeatTreasure());
        TestEqual(TEXT("Saved selected item identity survives exactly"), Drops[0]->GetItemType(), Original.Type);
        TestEqual(TEXT("Saved custom ammo family survives"), Drops[0]->GetAmmoIndex(), Original.AmmoIndex);
        TestEqual(TEXT("Saved custom quantity survives"), Drops[0]->GetAmmoQuantity(), Original.AmmoQuantity);
        TestTrue(TEXT("Saved bottom-rooted position survives"), Drops[0]->GetActorLocation().Equals(Original.Position, 0.001));
        if (QuestDrop) { TestTrue(TEXT("Reopening retains the separate authored quest drop"), QuestDrop->IsAvailable()); }
        for (ANativeWadPickup* Drop : Drops) { Drop->Destroy(); }
        FWadLevelCheckpoint Removed;
        if (!TestTrue(TEXT("Archive the defeated life after its ordinary pickup is gone"), Level->WriteCheckpoint(Removed))) { return false; }
        TestEqual(TEXT("Removed ordinary treasure is not archived as a remaining pickup"), Removed.DroppedItems.Num(), 0);
        Creature->ResetForGenerator();
        if (!TestTrue(TEXT("Reopen the checkpoint without the removed ordinary item"), Level->RestoreCheckpoint(Removed))) { return false; }
        Creature->TakeHit(1000);
        TestTrue(TEXT("Loading an already looted corpse never rerolls its treasure"),
            Creature->HasHandledDefeatTreasure() && OrdinaryDrops(*World).IsEmpty());
        const int32 PreviousLife = Creature->GetCreatureLife();
        Creature->ResetForGenerator();
        Creature->SetSpawned(true);
        TestTrue(TEXT("Generator reuse begins a genuinely new creature life"), Creature->GetCreatureLife() > PreviousLife);
        if (!TestTrue(TEXT("The regenerated life is defeated through its source resistance rules"),
                Creature->TakeHit(Profile.MaxHealth, nullptr, DefeatDamage))
            || !TestTrue(TEXT("The regenerated life really reached defeat"), Creature->IsDefeated())) { return false; }
        TestEqual(TEXT("The reused life may release exactly one fresh ordinary item"), OrdinaryDrops(*World).Num(), 1);
        for (ANativeWadPickup* Drop : OrdinaryDrops(*World)) { Drop->Destroy(); }
        if (QuestDrop) { QuestDrop->Destroy(); }
        Creature->Destroy();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalOrdinaryTreasureCollection,
    "AAOriginal.WAD.Gameplay.OrdinaryTreasureCollection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalOrdinaryTreasureCollection::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Engine for a real campaign game instance"), GEngine)) { return false; }
    UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
    Instance->InitializeStandalone();
    UWorld* World = Instance->GetWorld();
    ON_SCOPE_EXIT
    {
        Instance->Shutdown();
        if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
    };
    if (!TestNotNull(TEXT("World for actual loot collection"), World)) { return false; }
    UClass* TravelClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadTravelState"));
    auto* Travel = TravelClass ? static_cast<UNativeWadTravelState*>(Instance->GetSubsystemBase(TravelClass)) : nullptr;
    ANativeWadLevel* Level = World->SpawnActor<ANativeWadLevel>();
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>();
    if (!TestNotNull(TEXT("Initialized normal campaign purse"), Travel) || !Level || !Player || !InstallTreasureFloor(*Level)) { return false; }
    TSet<int32> Items;
    for (const int32 Source : OrdinarySources)
    {
        for (uint16 Roll = 0; Roll < 12; ++Roll) { Items.Add(ResolveWadCreatureTreasure(Source, Roll).ItemType); }
    }
    for (const int32 Type : Items)
    {
        ANativeWadPickup* Drop = ANativeWadPickup::SpawnDropped(*Level, Type, FVector(0, 0, 32));
        if (!TestNotNull(FString::Printf(TEXT("Loot branch %d has usable catalog rules and world art"), Type), Drop)) { return false; }
        const UStaticMeshComponent* Art = Drop->FindComponentByClass<UStaticMeshComponent>();
        TestTrue(TEXT("A cached inventory icon cannot conceal missing world art"), Art && Art->GetMaterial(0));
        const int32 InventoryBefore = Player->GetItemCount(Type);
        const int64 PurseBefore = Travel->Quest.GetWalletCopper();
        TestTrue(TEXT("The real pickup uses normal collection"), Drop->Collect(*Player));
        const int32 Coin = NativeWadRules::CoinValueCopper(Type);
        TestEqual(TEXT("Currency goes only to the campaign purse"), Travel->Quest.GetWalletCopper(), PurseBefore + Coin);
        TestEqual(TEXT("Non-currency retains its exact full inventory identity"), Player->GetItemCount(Type), InventoryBefore + (Coin == 0 ? 1 : 0));
    }
    // Explicit source AccData overrides the catalog's ordinary full-quiver size.
    const FWadCreatureTreasure Quivers[] = {{144, 0, 12}, {147, 2, 6}, {145, 3, 6}, {144, 0, 6}};
    for (const FWadCreatureTreasure& Quiver : Quivers)
    {
        ANativeWadPickup* Drop = ANativeWadPickup::SpawnDropped(*Level, Quiver.ItemType, FVector(0, 0, 32));
        if (!Drop) { return false; }
        Drop->SetAuthoredAmmo(Quiver.AmmoIndex, Quiver.AmmoQuantity);
        const int32 Before = Player->GetAmmoCount(Quiver.AmmoIndex);
        TestTrue(TEXT("Source-sized creature quivers collect as ammunition"), Drop->Collect(*Player));
        TestEqual(TEXT("Collection preserves the exact source ammo family and amount"),
            Player->GetAmmoCount(Quiver.AmmoIndex), Before + Quiver.AmmoQuantity);
    }
    return true;
}
