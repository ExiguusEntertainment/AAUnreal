#include "Misc/AutomationTest.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr int32 OrdinarySources[] = {1002, 1006, 1008, 1009, 1011, 1014, 1018, 1019,
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
        const auto Exact = [&](int32 Type, int32 Item, int32 Ammo = INDEX_NONE, int32 Count = 0, int32 Accessory = INDEX_NONE)
        {
            const FWadCreatureTreasure Result = ResolveWadCreatureTreasure(Type, Roll);
            if (Result.ItemType == Item && Result.AmmoIndex == Ammo && Result.AmmoQuantity == Count
                && Result.AccessoryData == Accessory && Result.HasAccessoryData() == (Accessory != INDEX_NONE)) { return true; }
            AddError(FString::Printf(TEXT("Source %d, input %d: got item/ammo/quantity/auxiliary %d/%d/%d/%d, expected %d/%d/%d/%d"),
                Type, Input, Result.ItemType, Result.AmmoIndex, Result.AmmoQuantity, Result.AccessoryData, Item, Ammo, Count, Accessory));
            return false;
        };
        if (!Exact(1002, (Roll & 1) ? 102 : 362)
            || !Exact(1006, (Roll & 1) ? 373 : 104, INDEX_NONE, 0, (Roll & 1) ? 407 : INDEX_NONE) || !Exact(1008, 147, 2, 6) || !Exact(1009, 144, 0, 12)
            || !Exact(1011, Trojan[Roll % 12]) || !Exact(1014, 145, 3, 6)
            || !Exact(1032, 416) || !Exact(1035, BadKnight[Roll % 12]) || !Exact(1036, 106)
            || !Exact(3000, 101) || !Exact(3001, 102) || !Exact(3002, 102) || !Exact(3003, 101)
            || !Exact(4080, EvilKnight[Roll % 12]) || !Exact(50158, 417)) { return false; }
    }
    for (int32 Type : {0, 1012, 1015, 1037, 37866, 37886})
    {
        TestEqual(TEXT("Source no-drop cases and unsupported identities gain no guessed treasure"),
            ResolveWadCreatureTreasure(Type, 1).ItemType, 0);
    }
    TestTrue(TEXT("Elymas's full identity does not collapse to the Orange Wizard journal table"),
        ResolveWadCreatureTreasure(50158, 1).ItemType == 417 && ResolveWadCreatureTreasure(50158 & 0xfff, 1).ItemType == 373
            && ResolveWadCreatureTreasure(50158, 1).AccessoryData == INDEX_NONE
            && ResolveWadCreatureTreasure(1006, 1).AccessoryData == 407);
    const FWadCreatureTreasure RecordedZero{373, INDEX_NONE, 0, 0};
    TestTrue(TEXT("Treasure payload zero is explicitly recorded rather than absent"), RecordedZero.HasAccessoryData());
    TestFalse(TEXT("Ordinary unadorned treasure has no implicit source payload"), FWadCreatureTreasure{104}.HasAccessoryData());
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
                TestEqual(TEXT("Actual defeat retains source auxiliary presence"), Drops[0]->HasAccessoryData(), Expected.HasAccessoryData());
                if (Expected.HasAccessoryData())
                { TestEqual(TEXT("Actual defeat retains its exact source auxiliary payload"), int32(Drops[0]->GetAccessoryData()), Expected.AccessoryData); }
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
        TestEqual(TEXT("Saved source auxiliary presence survives"), Drops[0]->HasAccessoryData(), Original.bAccessoryDataRecorded);
        TestEqual(TEXT("Saved source auxiliary value survives"), Drops[0]->GetAccessoryData(), Original.AccessoryData);
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
    TestEqual(TEXT("Every source branch contributes twenty-seven distinct ordinary objects"), Items.Num(), 27);
    for (const int32 Type : Items)
    {
        ANativeWadPickup* Drop = ANativeWadPickup::SpawnDropped(*Level, Type, FVector(0, 0, 32));
        if (!TestNotNull(FString::Printf(TEXT("Loot branch %d has usable catalog rules and world art"), Type), Drop)) { return false; }
        const UStaticMeshComponent* Art = Drop->FindComponentByClass<UStaticMeshComponent>();
        TestTrue(TEXT("A cached inventory icon cannot conceal missing world art"), Art && Art->GetMaterial(0));
        if (Type == 373)
        {
            const auto Treasure = ResolveWadCreatureTreasure(1006, 1);
            if (!TestTrue(TEXT("The original Orange Wizard branch sets page407 before GET"), Drop->SetAccessoryData(Treasure.AccessoryData))) { return false; }
        }
        const int32 InventoryBefore = Player->GetItemCount(Type);
        const int64 PurseBefore = Travel->Quest.GetWalletCopper();
        const int32 AmmoIndex = Drop->GetAmmoIndex();
        const int32 AmmoQuantity = Drop->GetAmmoQuantity();
        const int32 AmmoBefore = AmmoIndex == INDEX_NONE ? 0 : Player->GetAmmoCount(AmmoIndex);
        TestTrue(TEXT("The real pickup uses normal collection"), Drop->Collect(*Player));
        const int32 Coin = NativeWadRules::CoinValueCopper(Type);
        TestEqual(TEXT("Currency goes only to the campaign purse"), Travel->Quest.GetWalletCopper(), PurseBefore + Coin);
        TestEqual(TEXT("Ordinary objects retain their exact identity while GET ammunition bypasses the bag"),
            Player->GetItemCount(Type), InventoryBefore + (Coin == 0 && AmmoIndex == INDEX_NONE && Type != 373 ? 1 : 0));
        if (AmmoIndex != INDEX_NONE)
        { TestEqual(TEXT("A plain source quiver GET grants its effective ammunition quantity"), Player->GetAmmoCount(AmmoIndex), AmmoBefore + AmmoQuantity); }
        if (Type == 373)
        {
            TestTrue(TEXT("The actual catalog-backed GET stores original journal page407"), Travel->JournalPages.Contains(407));
            TestFalse(TEXT("A consumed journal cannot execute its GET twice"), Drop->Collect(*Player));
        }
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalOrangeWizardJournalLife,
    "AAOriginal.WAD.Gameplay.OrangeWizardJournalLife",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalOrangeWizardJournalLife::RunTest(const FString& Parameters)
{
    UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
    Instance->InitializeStandalone();
    UWorld* World = Instance->GetWorld();
    ON_SCOPE_EXIT
    {
        Instance->Shutdown();
        if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
    };
    if (!World) { return false; }
    UClass* TravelClass = FindObject<UClass>(nullptr, TEXT("/Script/AAOriginal.NativeWadTravelState"));
    auto* Travel = TravelClass ? static_cast<UNativeWadTravelState*>(Instance->GetSubsystemBase(TravelClass)) : nullptr;
    ANativeWadLevel* Level = World->SpawnActor<ANativeWadLevel>();
    ANativeWadGuardian* Wizard = World->SpawnActor<ANativeWadGuardian>(FVector(0,0,34), FRotator::ZeroRotator);
    ANativeWadPlayer* Player = World->SpawnActor<ANativeWadPlayer>(FVector(-200,0,60),FRotator::ZeroRotator);
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Travel || !Level || !Wizard || !Player || !Controller || !InstallTreasureFloor(*Level)) { return false; }
    World->AddController(Controller);
    Controller->Possess(Player);
    Controller->SetControlRotation(FRotator::ZeroRotator);
    Player->DispatchBeginPlay();
    Wizard->Tags.Add(TEXT("OrangeWizardJournalLifecycle"));
    FWadEncounterProfile Profile;
    Profile.SourceType = 1006;
    Profile.MaxHealth = 2000;
    Profile.CollisionHalfHeight = 32;
    Wizard->Configure(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,Profile);
    Wizard->RestoreEncounterHealth(Profile.MaxHealth);
    ANativeWadPickup* Page = nullptr;
    // Do not replace/seed the production RNG or force a particular loot result.
    // Each actual life must give one valid source branch. A bounded sequence
    // exercises the journal branch; exhaustive helper coverage above is deterministic.
    for (int32 Life = 0; Life < 128; ++Life)
    {
        if (!Wizard->TakeHit(Profile.MaxHealth) || !Wizard->IsDefeated()) { return false; }
        const auto Drops = OrdinaryDrops(*World);
        if (!TestEqual(TEXT("Each Orange Wizard life releases one source object"), Drops.Num(),1)) { return false; }
        if (Drops[0]->GetItemType()==373) { Page=Drops[0];break; }
        if (!TestEqual(TEXT("The other source branch is exactly a gold piece"), Drops[0]->GetItemType(),104)) { return false; }
        TestFalse(TEXT("Gold has no implicit journal payload"), Drops[0]->HasAccessoryData());
        const int64 Before=Travel->Quest.GetWalletCopper();
        if (!Drops[0]->Collect(*Player)) { return false; }
        TestEqual(TEXT("Actual Orange Wizard gold enters the normal purse"),Travel->Quest.GetWalletCopper(),Before+NativeWadRules::CoinValueCopper(104));
        TestEqual(TEXT("Gold never becomes a bag object"),Player->GetItemCount(104),0);
        Wizard->ResetForGenerator();
        Wizard->SetSpawned(true);
    }
    if (!TestNotNull(TEXT("A real ordinary Orange Wizard death produced its journal branch"),Page)) { return false; }
    TestTrue(TEXT("The defeat path records the exact original page407 payload"),Page->HasAccessoryData() && Page->GetAccessoryData()==407);
    Wizard->TakeHit(1000);
    Wizard->RestoreEncounterHealth(0);
    TestEqual(TEXT("Further hits/restored defeat cannot duplicate the journal"),OrdinaryDrops(*World).Num(),1);
    UNativeWadCheckpoint* Save=NewObject<UNativeWadCheckpoint>();
    if (!Level->WriteCheckpoint(Save->Level) || Save->Level.DroppedItems.Num()!=1) { return false; }
    TestTrue(TEXT("The real actor checkpoint explicitly records source page407"),Save->Level.DroppedItems[0].bAccessoryDataRecorded
        && Save->Level.DroppedItems[0].AccessoryData==407);
    TArray<uint8> Bytes;
    if (!UGameplayStatics::SaveGameToMemory(Save,Bytes)) { return false; }
    auto* Loaded=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromMemory(Bytes));
    if (!Loaded || !Level->RestoreCheckpoint(Loaded->Level)) { return false; }
    auto Drops=OrdinaryDrops(*World);
    if (!TestEqual(TEXT("SaveGame reopen restores only the existing journal object"),Drops.Num(),1)) { return false; }
    Page=Drops[0];
    TestTrue(TEXT("Reopened source payload remains present and exactly407"),Page->HasAccessoryData() && Page->GetAccessoryData()==407);
    TestTrue(TEXT("Reopened corpse retains once-per-life ordinary loot state"),Wizard->IsDefeated() && Wizard->HasHandledDefeatTreasure());
    Player->SetActorLocation(Page->GetActorLocation()-FVector(30,0,Player->BaseEyeHeight),false,nullptr,ETeleportType::TeleportPhysics);
    Controller->SetControlRotation(FRotator::ZeroRotator);
    const int64 PurseBefore=Travel->Quest.GetWalletCopper();
    if (!TestTrue(TEXT("Normal player Use executes the restored journal object's original GET"),Player->Use())) { return false; }
    TestTrue(TEXT("Actual source-catalog page407 is retained in the player's journal"),Travel->JournalPages.Contains(407));
    TestEqual(TEXT("Journal GET never adds373 to the inventory bag"),Player->GetItemCount(373),0);
    TestEqual(TEXT("Journal GET does not add or spend purse currency"),Travel->Quest.GetWalletCopper(),PurseBefore);
    TestFalse(TEXT("Consumed journal cannot execute GET twice"),Page->Collect(*Player));
    FWadLevelCheckpoint Removed;
    if (!Level->WriteCheckpoint(Removed)) { return false; }
    TestTrue(TEXT("Consumed journal is absent from subsequent world snapshots"),Removed.DroppedItems.IsEmpty());
    Wizard->ResetForGenerator();
    if (!Level->RestoreCheckpoint(Removed)) { return false; }
    Wizard->TakeHit(1000);
    TestTrue(TEXT("Reloading the looted dead life never rolls another object"),Wizard->HasHandledDefeatTreasure() && OrdinaryDrops(*World).IsEmpty());
    const int32 OldLife=Wizard->GetCreatureLife();
    Wizard->ResetForGenerator();
    Wizard->SetSpawned(true);
    if (!Wizard->TakeHit(Profile.MaxHealth)) { return false; }
    Drops=OrdinaryDrops(*World);
    TestTrue(TEXT("A genuinely regenerated wizard life can release one fresh object"),Wizard->GetCreatureLife()>OldLife && Drops.Num()==1);
    if (Drops.Num()!=1) { return false; }
    if (Drops[0]->GetItemType()==373)
    { TestTrue(TEXT("Regenerated journal again records407 rather than an absent/default payload"),Drops[0]->HasAccessoryData() && Drops[0]->GetAccessoryData()==407); }
    else { TestEqual(TEXT("Regenerated alternative remains the exact source gold identity"),Drops[0]->GetItemType(),104); }
    return true;
}
